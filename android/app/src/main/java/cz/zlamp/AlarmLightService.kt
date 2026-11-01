package cz.zlamp

import android.app.AlarmManager
import android.app.Notification
import android.content.*
import android.os.*
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import org.json.JSONObject
import java.time.Instant
import java.time.ZoneId

/** Only a live ringing notification near a published alarm time can trigger a morning. */
object MorningAlarm {
    const val SAMSUNG = "com.sec.android.app.clockpackage"
    fun inWindow(minute: Int, start: Int, end: Int): Boolean =
        if (start < end) minute in start until end else minute >= start || minute < end
    fun ringing(now: Long, posted: Long, due: Long, category: String?, fullScreen: Boolean, summary: Boolean): Boolean =
        category == Notification.CATEGORY_ALARM && fullScreen && !summary &&
            now - posted in 0..15000 && due > 0 && posted - due in -5000..120000
    fun observe(context: Context) {
        val prefs = (context.applicationContext as LampApp).alarmPrefs
        val next = context.getSystemService(AlarmManager::class.java).nextAlarmClock
        val now = System.currentTimeMillis(); val old = prefs.getLong("nextAt", 0)
        val edit = prefs.edit()
        // The clock often removes the alarm immediately before posting its ringing notification.
        if (old != next?.triggerTime && now - old in -5000..120000) {
            edit.putLong("recentAt", old).putString("recentPackage", prefs.getString("nextPackage", ""))
        }
        edit.putLong("nextAt", next?.triggerTime ?: 0).putString("nextPackage", next?.showIntent?.creatorPackage ?: "").apply()
    }
}

/** Android binds this opt-in listener. A bounded BLE command needs no permanent polling service. */
class AlarmLightService : NotificationListenerService() {
    private val app get() = application as LampApp
    private val prefs get() = app.alarmPrefs
    private val client get() = app.client
    private val handler = Handler(Looper.getMainLooper())
    private var registered = false
    private var active = false
    private var sent = false
    private var attempted = false
    private var address = ""
    private var offAt = 0L
    private var expires = 0L
    private var target = "all"
    private var level = 25
    private var wake: PowerManager.WakeLock? = null
    private val changed = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) { MorningAlarm.observe(context) }
    }
    override fun onListenerConnected() {
        super.onListenerConnected(); app.alarmListenerRunning = true
        if (!registered) {
            val filter = IntentFilter(AlarmManager.ACTION_NEXT_ALARM_CLOCK_CHANGED).apply {
                addAction(Intent.ACTION_TIME_CHANGED); addAction(Intent.ACTION_TIMEZONE_CHANGED)
            }
            if (Build.VERSION.SDK_INT >= 33) registerReceiver(changed, filter, Context.RECEIVER_EXPORTED)
            else @Suppress("DEPRECATION") registerReceiver(changed, filter)
            registered = true
        }
        MorningAlarm.observe(this)
        // Do not replay active notifications after process restart or permission changes.
    }
    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (!prefs.getBoolean("enabled", false) || sbn.packageName != prefs.getString("clockPackage", MorningAlarm.SAMSUNG)) return
        val now = System.currentTimeMillis(); val n = sbn.notification
        MorningAlarm.observe(this)
        val date = Instant.ofEpochMilli(now).atZone(ZoneId.systemDefault())
        val minute = date.hour * 60 + date.minute
        val candidates = listOf("next", "recent").mapNotNull { key ->
            prefs.getLong(key + "At", 0).takeIf { prefs.getString(key + "Package", "") == sbn.packageName }
        }
        // Keep only non-content diagnostics from the selected clock; never store notification text.
        prefs.edit().putString("diagnostic", "${n.category ?: "bez kategorie"} · celé okno ${n.fullScreenIntent != null} · čas budíku ${candidates.any { now-it in -5000..120000 }}").apply()
        if (!MorningAlarm.inWindow(minute, prefs.getInt("start", 300), prefs.getInt("end", 600))) return
        if (candidates.none { MorningAlarm.ringing(now, sbn.postTime, it, n.category, n.fullScreenIntent != null, n.flags and Notification.FLAG_GROUP_SUMMARY != 0) }) return
        if (prefs.getString("lastDay", "") == date.toLocalDate().toString() || active) return
        // Persist consumption before sending: updates, snoozes and restarts must never replay ON.
        if (!prefs.edit().putString("lastDay", date.toLocalDate().toString()).putLong("lastRing", sbn.postTime)
                .putString("lastResult", "Budík zazvonil, připojuji lampu…").commit()) return
        try {
            require(ArrivalService.permitted(this) && client.bluetoothEnabled) { "Zapni a povol Bluetooth telefonu." }
            address = prefs.getString("address", "").orEmpty()
            require(address.isNotBlank() && address == client.savedAddress && client.isBonded(address)) { "Nejdřív připoj uloženou lampu a ulož nastavení budíku." }
            require(!client.scanning && !client.pairing) { "Probíhá párování lampy." }
            target = prefs.getString("target", "all") ?: "all"
            level = (prefs.getInt("brightness", 10).coerceIn(1, 100) * 254 + 50) / 100
            offAt = sbn.postTime + prefs.getInt("minutes", 60).coerceIn(1, 1440) * 60000L
            active = true; sent = false; attempted = false; app.alarmBusy = true
            expires = SystemClock.elapsedRealtime() + 60000
            wake = getSystemService(PowerManager::class.java).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Zlamp:Alarm").apply { acquire(65000) }
            handler.post(command)
        } catch (e: RuntimeException) { finish(e.message ?: "Budík nemohl připojit lampu.") }
    }
    private val command = object : Runnable {
        override fun run() {
            if (!active) return
            if (!prefs.getBoolean("enabled", false)) { finish("Ranní světlo vypnuté."); return }
            if (SystemClock.elapsedRealtime() >= expires) { finish("Lampa neodpověděla. Ranní příkaz se později neopakuje."); return }
            if (sent || app.tileBusy) { handler.postDelayed(this, 200); return }
            try {
                require(!client.scanning && !client.pairing && client.savedAddress == address) { "Výběr lampy se změnil." }
                if (!client.ready && !client.connecting) {
                    require(!attempted) { client.status }
                    attempted = true; app.alarmOwnsConnection = true; client.connect(address, autoConnect = true)
                }
                require(client.connectionAddress == address) { "Je připojená jiná lampa." }
                if (client.ready) {
                    require(client.info.optInt("timers") >= 1) { "Aktualizuj Wi-Fi firmware na 2.10.0 nebo novější." }
                    val seconds = ((offAt - System.currentTimeMillis() + 999) / 1000).toInt()
                    require(seconds in 1..86400) { "Doba ranního svícení už uplynula." }
                    app.arrival.manualControl(); sent = true
                    client.request("timer", JSONObject().put("target", target).put("seconds", seconds).put("level", level)) { result ->
                        if (active) finish(result.fold({ "Rozsvíceno na ${prefs.getInt("brightness", 10)} %. Vypnutí hlídá lampa." }, { it.message ?: "Příkaz nebyl potvrzen." }))
                    }
                }
                if (active) handler.postDelayed(this, 200)
            } catch (e: RuntimeException) { finish(e.message ?: "Chyba ranního světla.") }
        }
    }
    private fun finish(message: String) {
        active = false; app.alarmBusy = false; handler.removeCallbacksAndMessages(null)
        prefs.edit().putString("lastResult", message).apply()
        if (wake?.isHeld == true) wake?.release()
        app.releaseAlarmConnection(); app.releaseTileConnection()
    }
    override fun onListenerDisconnected() {
        app.alarmListenerRunning = false
        if (active) finish("Přístup k oznámením byl přerušen.")
        super.onListenerDisconnected()
    }
    override fun onDestroy() {
        if (registered) unregisterReceiver(changed)
        app.alarmListenerRunning = false
        if (active) finish("Sledování budíku bylo ukončeno.")
        super.onDestroy()
    }
}
