package cz.zlamp

/** A visit is consumed before sending ON. No threshold crossing can repeat it. */
class ArrivalGate {
    var connected = false; private set
    var consumed = false; private set
    private var disconnectedAt: Long? = null
    private var aboveSince: Long? = null
    private var lastSample: Long? = null

    fun connect(now: Long, disconnectGraceMs: Long) {
        if (connected) return
        val away = disconnectedAt
        if (away == null || now - away >= disconnectGraceMs) consumed = false
        connected = true; resetSignal()
    }
    fun disconnect(now: Long) {
        if (connected) { connected = false; disconnectedAt = now }
        resetSignal()
    }
    fun resetSignal() { aboveSince = null; lastSample = null }
    fun manualControl() { if (connected) consumed = true; resetSignal() }
    fun sample(rssi: Int?, now: Long, threshold: Int, dwellMs: Long): Boolean {
        if (!connected || rssi == null || rssi !in -120..-1) { resetSignal(); return false }
        if (lastSample?.let { now - it > 3500 || now <= it } == true) resetSignal()
        lastSample = now
        if (consumed || rssi < threshold) { aboveSince = null; return false }
        if (aboveSince == null) aboveSince = now
        // Zero fires on this sample; positive durations require fresh, strong samples.
        if (now - aboveSince!! < dwellMs) return false
        consumed = true
        return true
    }
}
