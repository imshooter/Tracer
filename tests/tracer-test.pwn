#include <open.mp>
#include <tracer>

/**
 * Every report of Tracer in one run, each started from its own timer: an error inside CallLocalFunction,
 * OnRuntimeError suppressing one, the backtrace natives, a long call, an error repeated twenty times,
 * a recursion that never ends and a call stuck in a loop for four seconds. Run with error_repeat 1 and
 * hang_time 3000.
 */

static
    gArray[4],
    gIndex = 6,
    gCounter,
    gRuntimeErrors,
    bool:gSuppress,
    gRepeatTimer
;

main() {}

stock Deepest(index) {
    return gArray[index];
}

stock Sum(const values[3], count) {
    new
        total
    ;

    for (new i; i != count; ++i) {
        total += values[i];
    }

    return total;
}

stock Busy(milliseconds) {
    new
        start = GetTickCount()
    ;

    for (;;) {
        for (new i; i != 1000000; ++i) {
            gCounter += i;
        }

        if (GetTickCount() - start >= milliseconds) {
            break;
        }
    }
}

stock Recurse(depth) {
    return Recurse(depth + 1) + 1;
}

public OnRuntimeError(code, &bool:suppress) {
    ++gRuntimeErrors;
    printf("[test] OnRuntimeError(%i), suppress %i", code, gSuppress);
    suppress = gSuppress;

    return 1;
}

forward @Nested_Inner(value);
public @Nested_Inner(value) {
    new
        values[3] = { 1, 2, 3 }
    ;

    return Sum(values, value);
}

forward @Test_Nested();
public @Test_Nested() {
    printf("[test] nested start");
    CallLocalFunction("@Nested_Inner", "i", gIndex);
    printf("[test] nested goes on after the inner error");

    return 1;
}

forward @Test_Suppress();
public @Test_Suppress() {
    gSuppress = true;
    Deepest(gIndex + 1);

    return 1;
}

forward @Test_Backtrace();
public @Test_Backtrace() {
    new
        text[512]
    ;

    gSuppress = false;
    PrintBacktrace();
    GetBacktrace(text);
    printf("[test] GetBacktrace gave %i characters, first line: %.40s", strlen(text), text);
    PrintNativeBacktrace();

    return 1;
}

forward @Test_LongCall();
public @Test_LongCall() {
    Busy(20);
    printf("[test] long call done");

    return 1;
}

forward @Repeat_Error();
public @Repeat_Error() {
    return gArray[gIndex];
}

forward @Test_Repeat();
public @Test_Repeat() {
    gRepeatTimer = SetTimer("@Repeat_Error", 50, true);

    return 1;
}

forward @Test_RepeatStop();
public @Test_RepeatStop() {
    KillTimer(gRepeatTimer);

    return 1;
}

forward @Test_Recursion();
public @Test_Recursion() {
    return Recurse(0);
}

forward @Test_Hang();
public @Test_Hang() {
    printf("[test] hang start");
    Busy(4000);
    printf("[test] hang done");

    return 1;
}

forward @Test_Exit();
public @Test_Exit() {
    printf("[test] the script saw %i run time errors", gRuntimeErrors);
    SendRconCommand("exit");

    return 1;
}

public OnGameModeInit() {
    SetTimer("@Test_Nested", 300, false);
    SetTimer("@Test_Suppress", 500, false);
    SetTimer("@Test_Backtrace", 700, false);
    SetTimer("@Test_LongCall", 900, false);
    SetTimer("@Test_Repeat", 1100, false);
    SetTimer("@Test_RepeatStop", 2120, false);
    SetTimer("@Test_Recursion", 2500, false);
    SetTimer("@Test_Hang", 3000, false);
    SetTimer("@Test_Exit", 8500, false);

    return 1;
}
