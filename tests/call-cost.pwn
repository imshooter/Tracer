#include <open.mp>

/**
 * The cost of a native call and of a public started with CallLocalFunction, two million and two hundred
 * thousand times, to compare a server without Tracer, with it, and with profiling on.
 */

static
    Float:gValue = 1.0
;

main() {}

forward @Bench_Empty();
public @Bench_Empty() {
    return 1;
}

forward @Bench_Run();
public @Bench_Run() {
    new
        start = GetTickCount()
    ;

    for (new i; i != 2000000; ++i) {
        gValue = floatabs(gValue);
    }

    new const
        natives = GetTickCount() - start
    ;

    start = GetTickCount();

    for (new i; i != 200000; ++i) {
        CallLocalFunction("@Bench_Empty", "");
    }

    new const
        publics = GetTickCount() - start
    ;

    printf("[bench] native call %.1f ns, CallLocalFunction %.1f ns", float(natives) * 1000000.0 / 2000000.0, float(publics) * 1000000.0 / 200000.0);
    SendRconCommand("exit");

    return 1;
}

public OnGameModeInit() {
    SetTimer("@Bench_Run", 500, false);

    return 1;
}
