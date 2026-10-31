#pragma once

// Starts the loopback-only TCP dev bridge (127.0.0.1:port) on a background thread. Fails soft (logs).
void StartBridge(int port);

// Removes every trace/watch, restores pending auto-reverts, releases held keys.
// full=false (process is terminating): only memory reverts + key release, no thread-suspending work.
void ShutdownAll(bool full, bool inLoaderLock = false);   // inLoaderLock: called from DllMain - never wait for threads there
