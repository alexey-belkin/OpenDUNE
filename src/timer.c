/** @file src/timer.c Timer routines. */

#include <stdlib.h>
#if !defined(_MSC_VER) && !defined(TOS) && !defined(__WATCOMC__)
	#include <sys/time.h>
#endif /* _MSC_VER */
#if defined(_WIN32)
	/* WIN32 */
	#define _WIN32_WINNT 0x0500
	#include <windows.h>
#elif defined(TOS)
	/* Atari TOS */
	#include <mint/sysbind.h>
	#include <mint/osbind.h>
	#include <mint/ostruct.h>
	#include <mint/sysvars.h>
#elif defined(__WATCOMC__)
	#include <time.h>
#elif defined(__HAIKU__)
	#include <OS.h>
#else
	/* Linux / Mac OS X / etc. */
	#if !defined(__USE_POSIX)
		#define __USE_POSIX
	#endif /* !__USE_POSIX */
	#include <signal.h>
#endif

#include "types.h"
#include "os/sleep.h"
#include "os/error.h"

#include "timer.h"
#include "mpturn.h"

/* Set by the game loop, because the thing that steps a match lives up there and
 * this file is meant to know only about clocks. */
static void (*s_matchPump)(void) = NULL;

void Timer_SetMatchPump(void (*pump)(void))
{
	s_matchPump = pump;
}



volatile uint32 g_timerGUI = 0;                                      /*!< Tick counter. Increases with 1 every tick when Timer 1 is enabled. Used for GUI. */
volatile uint32 g_timerGame = 0;                                     /*!< Tick counter. Increases with 1 every tick when Timer 2 is enabled. Used for game timing (units, ..). */
/*!< Tick counter for explosions and animations.  They used to be scheduled
 * against g_timerGUI, which is the wall clock: the crater an explosion leaves,
 * the ground tile an animation sets and the randoms both of them draw are
 * simulation, so on two machines running at two slightly different frame rates
 * they happened at two different game ticks.  Outside a match it advances with
 * g_timerGUI and nothing changes; in a match the simulation stepper owns it. */
volatile uint32 g_timerAnim = 0;

/**
 * Whether the animation clock belongs to the match rather than to the wall.
 *
 * MpTurn_IsActive() is not enough on its own: it only becomes true once the
 * turn loop begins, and a match is set up first -- the map, the houses, the
 * starting squad, the tick-zero dump.  That setup takes as long as the machine
 * takes, and the 60 Hz ticker keeps counting through it, so the two clients
 * reached their first turn with animation clocks tens of ticks apart.  A
 * constant offset mostly cancels, because everything animations schedule is
 * relative to this clock -- which is exactly why it went unnoticed, and why a
 * checksum over the animation slots would otherwise report a disagreement in
 * every match.  Claimed before the match is built and released when it ends.
 */
static bool s_animClockClaimed = false;

void Timer_ClaimAnimClock(bool claimed)
{
	s_animClockClaimed = claimed;
}
volatile uint32 g_timerInput = 0;                                    /*!< Tick counter. Increases with 1 every tick. Used for input timing. */
volatile uint32 g_timerSleep = 0;                                    /*!< Tick counter. Increases with 1 every tick. Used for sleeping. */
volatile uint32 g_timerTimeout = 0;                                  /*!< Tick counter. Decreases with 1 every tick when non-zero. Used to timeout. */

static uint16 s_timersActive = 0;


typedef struct TimerNode {
	uint32 usec_left;
	uint32 usec_delay;
	void (*callback)(void);
	bool callonce;
} TimerNode;

#if defined(_WIN32)
static HANDLE s_timerMainThread = NULL;
static HANDLE s_timerThread = NULL;
#endif /* _WIN32 */

static TimerNode *s_timerNodes = NULL;
static int s_timerNodeCount = 0;
static int s_timerNodeSize  = 0;

static uint32 s_timerLastTime;

static const uint32 s_timerSpeed = 1000000 / 120; /* Our timer runs at 120Hz */


uint32 Timer_GetTime(void)
{
#if defined(_MSC_VER)
	DWORD t;
	t = timeGetTime();
	return t;
#elif defined(TOS)
	/* use the 200 HZ system timer which has a 5ms granularity */
	return get_sysvar(_hz_200) * 5;
#elif defined(__WATCOMC__)
	return clock() * 1000 / CLOCKS_PER_SEC;
#else
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif /* _MSC_VER */
}

/**
 * Run the timer interrupt handler.
 */
static void Timer_InterruptRun(int arg)
{
	TimerNode *node;
	uint32 new_time, usec_delta, delta;
	int i;

	/* Lock the timer, to avoid double-calls */
	static bool timerLock = false;
	if (timerLock) return;
	timerLock = true;

	/* Calculate the time between calls */
	new_time   = Timer_GetTime();
	usec_delta = (new_time - s_timerLastTime) * 1000;
	s_timerLastTime = new_time;

	/* Walk all our timers, see which (and how often) it should be triggered */
	node = s_timerNodes;
	for (i = 0; i < s_timerNodeCount; i++, node++) {
		delta = usec_delta;

		/* No delay means: as often as possible, but don't worry about it */
		if (node->usec_delay == 0) {
			node->callback();
			continue;
		}

		if (node->callonce) {
			if (node->usec_left <= delta) {
				delta -= node->usec_left;
				node->usec_left = node->usec_delay;
				if(arg == 0) node->callback();
				while (node->usec_left <= delta) delta -= node->usec_left;
			}
		} else while (node->usec_left <= delta) {
			delta -= node->usec_left;
			node->usec_left = node->usec_delay;
			node->callback();
		}
		node->usec_left -= delta;
	}

	timerLock = false;
}

/*
 * Every modal screen in this game -- the build list, the mentat, a briefing --
 * is a loop of its own that spins on sleepIdle() until the player leaves it.
 * On one machine that is fine: the world may as well stop while somebody reads.
 * In a match it is not, because the world belongs to two people, and the one
 * who is not reading watches a frozen battle until the other closes the window.
 *
 * So the pump goes here rather than into each of those loops: they all already
 * come through this function, and none of them has to know a match is on.
 * mp.md section 5 calls this Mp_Pump().
 */
static void Timer_PumpMatch(void)
{
	if (MpTurn_IsActive() && s_matchPump != NULL) s_matchPump();
}

#if defined(TOS) || defined(DOS)
void SleepAndProcessBackgroundTasks(void)
{
	Timer_InterruptRun(0);
	Timer_PumpMatch();
}

#elif defined(__HAIKU__)
void SleepAndProcessBackgroundTasks(void)
{
	static bigtime_t s_nextTrigger = 0;
	int dontRunCallonce = 0;
	if (s_nextTrigger == 0) s_nextTrigger = system_time();

	while (snooze_until(s_nextTrigger, B_SYSTEM_TIMEBASE) != B_OK)
		/* We were interrupted, snooze again */
		;

	do {
		s_nextTrigger += s_timerSpeed;
		Timer_InterruptRun(dontRunCallonce);
		dontRunCallonce = 1;
	} while (system_time() - s_nextTrigger > 0);

	Timer_PumpMatch();
}

#elif !defined(_WIN32) || defined(WITH_SDL) || defined(WITH_SDL2)
#if defined(_WIN32)
static volatile int s_timer_count = 0;
#else
/* POSIX */
static volatile sig_atomic_t s_timer_count = 0;
#endif

static void Timer_Handler(int sig)
{
	VARIABLE_NOT_USED(sig);
	
	/* indicate that Timer_InterruptRun() should be executed */
	s_timer_count++;
}

void SleepAndProcessBackgroundTasks(void)
{
	while (s_timer_count == 0) {
#if defined(_WIN32)
		Sleep(2); /* TODO : use a semaphore */
#else
		/* POSIX */
		pause();	/* wait for a signal to happen */
		/* another signal can have been triggered,
		 * ALSA sound is using SIGIO for triggering callbacks */
#endif
	}
	/* timer signal SIGALRM has been triggered */
	if (s_timer_count > 1) {
		Warning("s_timer_count = %d\n", (int)s_timer_count);
	}
	s_timer_count = 0;
	Timer_InterruptRun(0);
	if(s_timer_count > 0) {
		/* one more iteration if SIGALRM has been triggered
		 * during Timer_InterruptRun() */
		s_timer_count = 0;
		Timer_InterruptRun(1);	/* don't run "callonce" timers */
	}
	Timer_PumpMatch();
}
#endif /* _WIN32 */

#if defined(_WIN32)
void CALLBACK Timer_InterruptWindows(LPVOID arg, BOOLEAN TimerOrWaitFired) {
	VARIABLE_NOT_USED(arg);
	VARIABLE_NOT_USED(TimerOrWaitFired);

	SuspendThread(s_timerMainThread);
#if defined(WITH_SDL) || defined(WITH_SDL2)
	s_timer_count++;
#else
	Timer_InterruptRun(0);
#endif /* defined(WITH_SDL) || defined(WITH_SDL2) */
	ResumeThread(s_timerMainThread);
}
#endif /* _WIN32 */

#if !defined(TOS) && !defined(DOS) && !defined(__HAIKU__)

/**
 * Suspend the timer interrupt handling.
 */
static void Timer_InterruptSuspend(void)
{
#if defined(_WIN32)
	if (s_timerThread != NULL) DeleteTimerQueueTimer(NULL, s_timerThread, NULL);
	s_timerThread = NULL;
#else
	struct itimerval timerTime;
	timerTime.it_value.tv_sec = 0;
	timerTime.it_value.tv_usec = 0;
	timerTime.it_interval.tv_sec = 0;
	timerTime.it_interval.tv_usec = 0;
	setitimer(ITIMER_REAL, &timerTime, NULL);
#endif /* _WIN32 */
}

/**
 * Resume the timer interrupt handling.
 */
static void Timer_InterruptResume(void)
{
#if defined(_WIN32)
	int timerTime = s_timerSpeed / 1000;
	CreateTimerQueueTimer(&s_timerThread, NULL, Timer_InterruptWindows, NULL, timerTime, timerTime, WT_EXECUTEINTIMERTHREAD);
#else
	struct itimerval timerTime;
	timerTime.it_value.tv_sec = 0;
	timerTime.it_value.tv_usec = s_timerSpeed;
	timerTime.it_interval.tv_sec = 0;
	timerTime.it_interval.tv_usec = s_timerSpeed;
	setitimer(ITIMER_REAL, &timerTime, NULL);
#endif /* _WIN32 */
}

#endif /* !defined(TOS) */

/**
 * Initialize the timer.
 */
void Timer_Init(void)
{
	s_timerLastTime = Timer_GetTime();

#if defined(_WIN32)
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_timerMainThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
#elif defined(TOS)
	(void)Cconws("Timer_Init()\r\n");
	/* see http://toshyp.atari.org/en/004009.html#Xbtimer */
	/* s_timerSpeed * 614400 / 10000 */
#if 0
	Xbtimer(0/* Timer A */, 1/* divider = 4 */, s_timerSpeed * 6144 / 10000, Timer_Handler);
#endif
#elif defined(DOS) || defined(__HAIKU__)
	/* */
#else
	{
		struct sigaction timerSignal;

		sigemptyset(&timerSignal.sa_mask);
		timerSignal.sa_handler = Timer_Handler;
		timerSignal.sa_flags   = 0;
		sigaction(SIGALRM, &timerSignal, NULL);
	}
#endif /* _WIN32 */
#if !defined(TOS) && !defined(DOS) && !defined(__HAIKU__)
	Timer_InterruptResume();
#endif /* !defined(TOS) && !defined(DOS) */
}

/**
 * Uninitialize the timer.
 */
void Timer_Uninit(void)
{
#if !defined(TOS) && !defined(DOS) && !defined(__HAIKU__)
	Timer_InterruptSuspend();
#endif /* !defined(TOS) && !defined(DOS) */
#if defined(_WIN32)
	CloseHandle(s_timerMainThread);
#endif /* _WIN32 */

	free(s_timerNodes); s_timerNodes = NULL;
	s_timerNodeCount = 0;
	s_timerNodeSize = 0;
}

/**
 * Add a timer.
 * @param callback the callback for the timer.
 * @param usec_delay The interval of the timer.
 */
void Timer_Add(void (*callback)(void), uint32 usec_delay, bool callonce)
{
	TimerNode *node;
	if (s_timerNodeCount == s_timerNodeSize) {
		s_timerNodeSize += 2;
		s_timerNodes = (TimerNode *)realloc(s_timerNodes, s_timerNodeSize * sizeof(TimerNode));
	}
	node = &s_timerNodes[s_timerNodeCount++];

	node->usec_left  = usec_delay;
	node->usec_delay = usec_delay;
	node->callback   = callback;
	node->callonce   = callonce;
}

/**
 * Change the interval of a timer.
 * @param callback The callback to change the timer of.
 * @param usec_delay The interval.
 */
void Timer_Change(void (*callback)(void), uint32 usec_delay)
{
	int i;
	TimerNode *node = s_timerNodes;
	for (i = 0; i < s_timerNodeCount; i++, node++) {
		if (node->callback == callback) {
			node->usec_delay = usec_delay;
			return;
		}
	}
}

/**
 * Remove a timer from the queue.
 * @param callback Which callback to remove.
 */
void Timer_Remove(void (*callback)(void))
{
	int i;
	TimerNode *node = s_timerNodes;
	for (i = 0; i < s_timerNodeCount; i++, node++) {
		if (node->callback == callback) {
			*node = s_timerNodes[--s_timerNodeCount];
			return;
		}
	}
}

/**
 * Handle game timers.
 */
void Timer_Tick(void)
{
	if ((s_timersActive & TIMER_GUI)  != 0) {
		g_timerGUI++;
		/* In a match this one comes from the stepper instead. */
		if (!MpTurn_IsActive() && !s_animClockClaimed) g_timerAnim++;
	}
	if ((s_timersActive & TIMER_GAME) != 0) g_timerGame++;
	g_timerInput++;
	g_timerSleep++;

	if (g_timerTimeout != 0) g_timerTimeout--;
}

/** Advance only game simulation time by one tick for Fast x2 mode. */
void Timer_AdvanceGame(void)
{
	if ((s_timersActive & TIMER_GAME) != 0) g_timerGame++;
}

/**
 * Advance game simulation time by one tick, whether or not the automatic ticker
 * is running.
 *
 * The automatic ticker is driven by the wall clock, which makes the absolute
 * value of g_timerGame depend on how long a run took.  Anything that has to be
 * reproducible -- a checksummed run today, the network stepper later (mp.md) --
 * switches TIMER_GAME off and owns the clock through this instead.
 */
void Timer_StepGame(void)
{
	g_timerGame++;
}

/** Restart the game clock at zero, for a run that owns it. */
void Timer_ResetGame(void)
{
	g_timerGame = 0;
}

/**
 * The same pair for the GUI clock.
 *
 * Explosions and animations are scheduled against g_timerGUI and they write to
 * the map, so a run that has to be reproducible has to own this clock too.  See
 * mp.md -- in a match the two clocks have to become one.
 */
void Timer_StepGUI(void)
{
	g_timerGUI++;
	g_timerAnim++;
}

void Timer_ResetGUI(void)
{
	g_timerGUI = 0;
	g_timerAnim = 0;
}

/**
 * Set timers on and off.
 *
 * @param timer The timer to switch.
 * @param set True sets the timer on, false sets it off.
 * @return True if timer was set, false if it was not set.
 */
bool Timer_SetTimer(TimerType timer, bool set)
{
	uint8 t;
	bool ret;

	t = (1 << (timer - 1));
	ret = (s_timersActive & t) != 0;

	/* A networked match owns the simulation clock, and nothing is allowed to
	 * hand it back to the wall.  Half a dozen places switch this ticker on when
	 * they finish with a screen -- GUI_ChangeSelectionType() on the way into the
	 * game, the factory window, the placement mode -- and every one of them would
	 * put g_timerGame back on each machine's own 60 Hz, which is two different
	 * clocks for one world.  It costs exactly one tick of drift to desync, and
	 * that is what it cost. */
	if (timer == TIMER_GAME && set && MpTurn_IsActive()) return ret;

	if (set) {
		s_timersActive |= t;
	} else {
		s_timersActive &= ~t;
	}

	return ret;
}

/**
 * Sleep for an amount of ticks.
 * @param ticks The amount of ticks to sleep.
 */
void Timer_Sleep(uint16 ticks)
{
	uint32 tick = g_timerSleep + ticks;
	while (tick >= g_timerSleep) sleepIdle();
}
