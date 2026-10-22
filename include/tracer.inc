#if defined _INC_TRACER
    #endinput
#endif
#define _INC_TRACER

/**
 * Natives and callbacks of the Tracer component.
 *
 * Tracer reports run time errors with the place and the backtrace, calls that take longer than
 * `tracer.long_call_time`, calls that never return and crashes of the server, and profiles the
 * scripts when `tracer.profile` is on.
 *
 * The natives and the callback of crashdetect keep their names here, so a script written for
 * crashdetect works unchanged. Its long call stocks call the natives of this file.
 */

/**
 * # Long calls
 */

/**
 * @brief      Set how long a top level call may run before it is reported, for every script.
 *
 * @param      microseconds  The time, or `0` to stop reporting long calls.
 */
native Tracer_SetLongCallTime(microseconds);

/**
 * @brief      How long a top level call may run before it is reported.
 *
 * @return     The time in microseconds.
 */
native Tracer_GetLongCallTime();

/**
 * @brief      How long a top level call may run as `tracer.long_call_time` sets it.
 *
 * @return     The time in microseconds.
 */
native Tracer_GetDefaultLongCallTime();

/**
 * @brief      Go back to the time `tracer.long_call_time` sets.
 */
native Tracer_ResetLongCallTime();

/**
 * @brief      Report long calls again after `Tracer_DisableLongCall`.
 */
native Tracer_EnableLongCall();

/**
 * @brief      Stop reporting long calls until `Tracer_EnableLongCall`.
 */
native Tracer_DisableLongCall();

/**
 * @brief      Whether long calls are reported.
 *
 * @return     - `true` they are
 *             - `false` they are not, or the time is `0`
 */
native bool:Tracer_IsLongCallEnabled();

/**
 * @brief      Count the current top level call from now, so a task known to be long is not
 *             reported.
 */
native Tracer_RestartLongCall();

/**
 * # crashdetect
 */

#if !defined CRASHDETECT_INC
    #define CRASHDETECT_INC

    /**
     * @brief      Print the backtrace of the Pawn code that calls it.
     */
    native PrintBacktrace();

    /**
     * @brief      Print the backtrace of the native code that calls it.
     */
    native PrintNativeBacktrace();

    /**
     * @brief      Get the backtrace of the Pawn code that calls it, one frame per line.
     *
     * @param      string  Receives the backtrace.
     * @param      size    Size of `string`.
     *
     * @return     - `1` the backtrace was written
     *             - `0` it was not
     */
    native GetBacktrace(string[], size = sizeof (string));

    /**
     * @brief      Get the backtrace of the native code that calls it, one frame per line.
     *
     * @param      string  Receives the backtrace.
     * @param      size    Size of `string`.
     *
     * @return     - `1` the backtrace was written
     *             - `0` it was not
     */
    native GetNativeBacktrace(string[], size = sizeof (string));

    /**
     * @brief      Called when a run time error ends a call, before the error is printed.
     *
     * @param      code      The error, one of the `AMX_ERR_` codes.
     * @param      suppress  Set it to `true` to leave the error out of the log.
     */
    forward OnRuntimeError(code, &bool:suppress);

    stock bool:IsCrashDetectPresent() {
        return true;
    }

    stock SetCrashDetectLongCallTime(us_time) {
        return Tracer_SetLongCallTime(us_time);
    }

    stock GetCrashDetectLongCallTime() {
        return Tracer_GetLongCallTime();
    }

    stock DisableCrashDetectLongCall() {
        return Tracer_DisableLongCall();
    }

    stock EnableCrashDetectLongCall() {
        return Tracer_EnableLongCall();
    }

    stock ResetCrashDetectLongCallTime() {
        return Tracer_ResetLongCallTime();
    }

    stock RestartCrashDetectLongCall() {
        return Tracer_RestartLongCall();
    }

    stock bool:IsCrashDetectLongCallEnabled() {
        return Tracer_IsLongCallEnabled();
    }

    stock bool:HasCrashDetectLongCall() {
        return true;
    }

    stock GetCrashDetectDefaultTime() {
        return Tracer_GetDefaultLongCallTime();
    }
#endif
