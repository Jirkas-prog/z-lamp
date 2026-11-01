package cz.zlamp

import android.app.Application
import android.os.SystemClock

/** One GATT owner shared by the screen and the optional foreground service. */
class LampApp : Application() {
    val arrivalPrefs by lazy { getSharedPreferences("arrival", MODE_PRIVATE) }
    val arrival = ArrivalGate()
    var arrivalStatus = "Příchod je vypnutý."
    var arrivalRunning = false
    var uiVisible = false
    var companionPresent = false
    var choosingLamp = false
    var calibrating = false
    val alarmPrefs by lazy { getSharedPreferences("morningAlarm", MODE_PRIVATE) }
    var alarmBusy = false
    var alarmOwnsConnection = false
    var alarmListenerRunning = false
    fun releaseAlarmConnection() {
        if (alarmOwnsConnection && !alarmBusy && !tileBusy && !tileListening && !uiVisible && !companionPresent && !arrivalPrefs.getBoolean("enabled", false)) {
            alarmOwnsConnection = false; client.disconnect()
        }
    }
    val tilePrefs by lazy { getSharedPreferences("tile", MODE_PRIVATE) }
    var tileListening = false
    var tileBusy = false
    var tileOwnsConnection = false
    var tileOn: Boolean? = null
    var tileStatus = "Klepni pro připojení"
    fun releaseTileConnection() {
        if (tileOwnsConnection && !tileBusy && !alarmBusy && !tileListening && !uiVisible && !companionPresent && !arrivalPrefs.getBoolean("enabled", false)) {
            tileOwnsConnection = false; client.disconnect()
        }
    }
    var onLink: ((Boolean) -> Unit)? = null
    val client: BleClient by lazy {
        BleClient(this).also { ble ->
            ble.onLink = { ready ->
                if (ready) arrival.connect(SystemClock.elapsedRealtime(), arrivalPrefs.getInt("grace", 0) * 1000L)
                else arrival.disconnect(SystemClock.elapsedRealtime())
                onLink?.invoke(ready)
            }
        }
    }
}
