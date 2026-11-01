package cz.zlamp

import android.Manifest
import android.app.*
import android.content.*
import android.content.pm.PackageManager
import android.os.*
import org.json.JSONObject

/** Started by an explicit opt-in. No scan loop and no pairing prompts in the background. */
class ArrivalService : Service() {
    companion object {
        private const val CHANNEL = "arrival"
        private const val NOTIFICATION = 31
        private const val PAUSE = "cz.zlamp.PAUSE_ARRIVAL"
        fun permitted(context: Context) = Build.VERSION.SDK_INT < 31 ||
            context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
        fun start(context: Context) {
            require(permitted(context)) { "Povol Bluetooth pro automatický příchod." }
            context.startForegroundService(Intent(context, ArrivalService::class.java))
        }
        fun stop(context: Context) {
            (context.applicationContext as LampApp).arrivalPrefs.edit().putBoolean("enabled", false).apply()
            context.stopService(Intent(context, ArrivalService::class.java))
        }
    }
    private val app get() = application as LampApp
    private val prefs get() = app.arrivalPrefs
    private val client get() = app.client
    private val handler = Handler(Looper.getMainLooper())
    private var wake: PowerManager.WakeLock? = null
    private var wakeUntil = 0L
    private var retryAt = 0L
    private var retryDelay = 5000L
    private var notificationText = ""
    private var actionResult = ""
    internal var localMinute: () -> Int = { java.time.LocalTime.now().let { it.hour * 60 + it.minute } }
    private val tick = object : Runnable {
        override fun run() {
            if (!prefs.getBoolean("enabled", false)) { stopSelf(); return }
            if (!permitted(this@ArrivalService)) { app.arrivalStatus = "Chybí oprávnění Bluetooth. Otevři aplikaci."; stopSelf(); return }
            try { update() } catch (_: SecurityException) {
                client.disconnect("Chybí oprávnění Bluetooth."); app.arrivalStatus = "Povol Bluetooth v nastavení aplikace."; stopSelf(); return
            }
            handler.postDelayed(this, 1000)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null
    override fun onCreate() {
        super.onCreate()
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL, "Automatický příchod", NotificationManager.IMPORTANCE_LOW).apply {
                description = "Připojení k lampě a jednorázové rozsvícení při příchodu."; setShowBadge(false)
            })
        wake = getSystemService(PowerManager::class.java).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Zlamp:Arrival").apply { setReferenceCounted(false) }
    }
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == PAUSE) {
            prefs.edit().putBoolean("enabled", false).apply(); client.disconnect("Příchod pozastaven.")
            stopForeground(STOP_FOREGROUND_REMOVE); stopSelf(); return START_NOT_STICKY
        }
        if (!prefs.getBoolean("enabled", false) || !permitted(this)) { stopSelf(); return START_NOT_STICKY }
        try { startForeground(NOTIFICATION, notification("Čekám na uloženou lampu…")) }
        catch (_: RuntimeException) { app.arrivalStatus = "Otevři aplikaci a ověř oprávnění pro Příchod."; stopSelf(); return START_NOT_STICKY }
        notificationText = ""
        app.arrivalRunning = true
        app.onLink = { ready ->
            releaseWake()
            if (ready) { retryDelay = 5000; actionResult = "" }
            else { retryAt = SystemClock.elapsedRealtime() + retryDelay; retryDelay = (retryDelay * 2).coerceAtMost(60000) }
            handler.removeCallbacks(tick); handler.post(tick)
        }
        client.onSignal = { rssi ->
            if (prefs.getBoolean("enabled", false) && !app.calibrating && !app.tileBusy && !app.alarmBusy && client.connectionAddress == prefs.getString("address", "")) {
                if (app.arrival.sample(rssi, SystemClock.elapsedRealtime(), prefs.getInt("threshold", -60), prefs.getInt("dwell", 0).coerceAtLeast(0) * 1000L)) {
                    // Latch first, then submit once. A timeout never retries a possibly successful ON.
                    releaseWake(); actionResult = "Odesílám rozsvícení…"; publish(actionResult)
                    val address = client.connectionAddress
                    val reply: (Result<JSONObject>) -> Unit = { result ->
                        if (client.connectionAddress == address) {
                            actionResult = result.fold({ "Příchod vyřízen · příkaz potvrzen bránou." },
                                { "Příchod vyřízen · ${it.message ?: "příkaz nepotvrzen"}" })
                            prefs.edit().putString("lastResult", actionResult).putLong("lastTime", System.currentTimeMillis()).apply()
                            if (client.ready) publish(actionResult)
                        }
                    }
                    if (!prefs.getBoolean("nightEnabled", true)) {
                        client.control(prefs.getString("target", "all") ?: "all", JSONObject().put("on", true), reply)
                    } else {
                        try {
                            require(client.info.optInt("arrival") >= 1) { "Noční režim vyžaduje firmware Wi-Fi 2.9.0 a C6 2.7.1." }
                            val plan = ArrivalSchedule(prefs.getInt("evening", 1260), prefs.getInt("night", 1380), prefs.getInt("morning", 420),
                                prefs.getInt("dayPercent", 100), prefs.getInt("nightPercent", 1)).at(localMinute())
                            val night = plan.phase == "night"
                            val target = if (night) prefs.getString("nightTarget", "").orEmpty() else prefs.getString("target", "all") ?: "all"
                            require(!night || Regex("[0-9a-fA-F]{16}").matches(target)) { "Vyber jednu žárovku pro noční příchod." }
                            client.request("arrival", JSONObject().put("target", target).put("level", plan.level)
                                .put("offSeconds", if (night) prefs.getInt("nightMinutes", 5) * 60 else 0)
                                .put("kelvin", if (night) prefs.getInt("nightKelvin", 2700) else 0), reply)
                        } catch (e: IllegalArgumentException) { reply(Result.failure(e)) }
                    }
                }
            } else app.arrival.resetSignal()
        }
        handler.removeCallbacks(tick); handler.post(tick)
        return START_STICKY
    }
    private fun update() {
        if (app.choosingLamp) { releaseWake(); return }
        val address = prefs.getString("address", "").orEmpty()
        if (address.isBlank() || address != client.savedAddress) {
            publish("Vyber uloženou lampu v nastavení Příchodu."); releaseWake(); return
        }
        if (!client.bluetoothEnabled) {
            if (client.ready || client.connecting) client.disconnect("Bluetooth telefonu je vypnuté.")
            publish("Čekám na zapnutí Bluetooth telefonu."); releaseWake(); return
        }
        if (!client.isBonded(address)) { publish("Obnov párování lampy v aplikaci."); releaseWake(); return }
        if (client.ready && client.connectionAddress == address) {
            val gate = app.arrival
            val consumed = gate.consumed
            if (!consumed && !app.calibrating) {
                // Only keep the CPU awake between connecting and this visit's arrival.
                // Timed leases make a stopped/crashed polling loop release the lock automatically.
                val now = SystemClock.elapsedRealtime()
                if (now + 20000 >= wakeUntil) { wake?.acquire(60000); wakeUntil = now + 60000 }
            } else releaseWake()
            if (!consumed || app.calibrating) client.readSignal()
            publish(when {
                app.calibrating -> "Kalibrace · automatické rozsvícení pozastaveno."
                consumed -> actionResult.ifBlank { "Příchod vyřízen · další až po odpojení." }
                else -> "Připojeno · čekám na silnější signál."
            })
        } else {
            releaseWake()
            if (!client.connecting && !client.scanning && SystemClock.elapsedRealtime() >= retryAt) {
                retryAt = SystemClock.elapsedRealtime() + retryDelay
                retryDelay = (retryDelay * 2).coerceAtMost(60000)
                client.connect(address, autoConnect = true)
            }
            publish("Čekám na lampu v dosahu · automatické připojení.")
        }
    }
    private fun publish(text: String) {
        app.arrivalStatus = text
        if (notificationText != text) {
            notificationText = text
            if (Build.VERSION.SDK_INT < 33 || checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED)
                getSystemService(NotificationManager::class.java).notify(NOTIFICATION, notification(text))
        }
    }
    private fun notification(text: String): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java).putExtra("arrival", true), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val pause = PendingIntent.getService(this, 1, Intent(this, ArrivalService::class.java).setAction(PAUSE), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL).setSmallIcon(R.drawable.ic_lamp).setContentTitle("Z-lamp · Příchod")
            .setContentText(text).setStyle(Notification.BigTextStyle().bigText(text))
            .setContentIntent(open).setOngoing(true).setOnlyAlertOnce(true)
            .addAction(Notification.Action.Builder(null, "Vypnout příchod", pause).build()).build()
    }
    private fun releaseWake() { if (wake?.isHeld == true) wake?.release(); wakeUntil = 0L }
    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null); releaseWake()
        client.onSignal = null; app.onLink = null; app.arrivalRunning = false
        app.arrival.resetSignal()
        if (!prefs.getBoolean("enabled", false)) app.arrivalStatus = "Příchod je vypnutý."
        if (!app.uiVisible && !app.companionPresent && !app.tileBusy && !app.alarmBusy && !app.tileListening) client.disconnect()
        super.onDestroy()
    }
}

/** Only restores a feature the user has already enabled. */
class ArrivalBootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action !in listOf(Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_MY_PACKAGE_REPLACED)) return
        val app = context.applicationContext as LampApp
        if (app.arrivalPrefs.getBoolean("enabled", false) && ArrivalService.permitted(context)) {
            try { ArrivalService.start(context) } catch (_: RuntimeException) { app.arrivalStatus = "Otevři aplikaci pro obnovení Příchodu." }
        }
    }
}
