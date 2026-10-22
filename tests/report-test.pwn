#include <open.mp>

/**
 * A gamemode that gives every part of the report something to show in four
 * seconds, then shuts the server down. Build it with `-d3`, run it as the main
 * script with `tracer.profile` and `tracer.profile_functions` set to `true`, and
 * the report is written as the server exits.
 *
 * - `Work` runs every 50 ms and calls two functions that do float math and string natives.
 * - `Spike` runs once and holds the server for 20 ms, so it heads the slowest ticks.
 * - `Fail` calls `Broken` through `CallLocalFunction` every 700 ms, and `Broken` writes out of bounds two functions deep.
 *
 * The log and the report show a run time error in `Broken` five times, at the
 * line of `StoreInternal`, with `StoreInternal`, `FillInternal`, `Broken` and
 * `CallLocalFunction` in its backtrace.
 */

/**
 * # Header
 */

static
    gSink
;

main() {}

/**
 * # Internal
 */

static stock Float:MixInternal(count) {
    new
        Float:sum
    ;

    for (new i; i != count; ++i) {
        sum = floatadd(sum, float(i));
    }

    return sum;
}

static stock DescribeInternal(Float:sum) {
    new
        text[64]
    ;

    format(text, sizeof (text), "%i %f", gSink, sum);

    return strlen(text);
}

static stock BusyInternal(milliseconds) {
    new const
        start = GetTickCount()
    ;

    while (GetTickCount() - start < milliseconds) {
        ++gSink;
    }
}

static stock StoreInternal(values[2], index, value) {
    values[index] = value;
}

static stock FillInternal(values[2], size) {
    StoreInternal(values, size, gSink);
}

forward Work();
public Work() {
    gSink += DescribeInternal(MixInternal(200));

    return 1;
}

forward Spike();
public Spike() {
    BusyInternal(20);

    return 1;
}

forward Broken();
public Broken() {
    new
        values[2]
    ;

    FillInternal(values, sizeof (values));

    return 1;
}

forward Fail();
public Fail() {
    CallLocalFunction("Broken", "");

    return 1;
}

forward Quit();
public Quit() {
    SendRconCommand("exit");

    return 1;
}

/**
 * # Calls
 */

public OnGameModeInit() {
    SetTimer("Work", 50, true);
    SetTimer("Spike", 1500, false);
    SetTimer("Fail", 700, true);
    SetTimer("Quit", 4000, false);

    return 1;
}
