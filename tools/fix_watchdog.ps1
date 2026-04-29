$file = 'c:\Users\n3Tk0\Documents\Arduino\ESP_Logger\src\tasks\TaskManager.cpp'
$content = [System.IO.File]::ReadAllText($file)
$marker = 'bool TaskManager::checkHealth()'
$idx = $content.IndexOf($marker)
if ($idx -lt 0) { Write-Error "Marker not found"; exit 1 }
$before = $content.Substring(0, $idx)
$replacement = @"
bool TaskManager::checkHealth() {
    if (!running) return true;
    // Grace period: skip watchdog checks for the first 60s after boot.
    // SDS011 in periodic mode may wait 60-90s for its first frame.
    constexpr uint32_t GRACE_PERIOD_MS = 60000;
    constexpr uint32_t MAX_SILENCE_MS  = 30000;
    uint32_t now = millis();
    if (now < GRACE_PERIOD_MS) return true;
    for (int i = 0; i < TASK_COUNT; i++) {
        uint32_t hb = g_taskHeartbeat[i];
        if (hb == 0) continue;   // task has not started yet
        if (now - hb > MAX_SILENCE_MS) {
            Serial.printf("[Watchdog] Task %d stuck (%lums)\n", i, now - hb);
            return false;
        }
    }
    return true;
}

"@
$newContent = $before + $replacement
[System.IO.File]::WriteAllText($file, $newContent)
Write-Host "Done - checkHealth() replaced"
