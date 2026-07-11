// This translation unit intentionally does not include Arduino headers. The
// Arduino core declares its default hook as weak; including that declaration
// here would make this definition weak too and allow immediate confirmation.
// A strong definition defers verification to ota_service after setup().
extern "C" bool verifyRollbackLater() {
    return true;
}

// Referenced by main.cpp so the linker retains this translation unit and its
// strong hook ahead of the Arduino core's weak default implementation.
bool (*volatile rollbackVerificationHook)() = verifyRollbackLater;
void ensureRollbackVerificationDeferral() {
    // This call is normally a no-op. Its purpose is to retain the hook through
    // section garbage collection, before the core consults it on future boots.
    if (!rollbackVerificationHook()) __builtin_trap();
}
