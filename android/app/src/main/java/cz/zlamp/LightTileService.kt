package cz.zlamp

import android.app.*
import android.content.*
import android.graphics.drawable.Icon
import android.os.*
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import android.widget.Toast
import org.json.JSONObject

/** Reads fresh gateway state before deciding ON/OFF; never toggles a remembered UI value. */
internal fun readTilePower(client: BleClient, target: String, reply: (Result<Boolean>) -> Unit) {
    val ids = linkedSetOf<String>()
    fun request(op: String, args: JSONObject, next: (JSONObject) -> Unit) {
        client.request(op, args) { result -> result.fold({ data ->
            try { next(data) } catch (e: Exception) { reply(Result.failure(e)) }
        }, { reply(Result.failure(it)) }) }
    }
    fun read(index: Int = 0, anyOn: Boolean = false, online: Int = 0) {
        if (index == ids.size) {
            reply(if (online > 0) Result.success(anyOn) else Result.failure(IllegalStateException("Vybraná světla nejsou dostupná."))); return
        }
        request("get_light", JSONObject().put("id", ids.elementAt(index))) { light ->
            if (!light.getBoolean("online")) read(index + 1, anyOn, online)
            else {
                val on = when {
                    light.optBoolean("requested") -> light.getJSONObject("wanted").getBoolean("on")
                    light.optBoolean("reported") -> light.getBoolean("on")
                    else -> error("Čekám na stav žárovky. Zkus to za chvíli.")
                }
                read(index + 1, anyOn || on, online + 1)
            }
        }
    }
    fun page(offset: Int = 0) {
        request("list_lights", JSONObject().put("offset", offset)) { data ->
            val items = data.getJSONArray("items"); require(items.length() <= 4)
            for (i in 0 until items.length()) items.getJSONObject(i).getString("id").let { if (it != "display") ids.add(it) }
            require(ids.size <= 16)
            if (data.isNull("next")) read() else {
                val next = data.getInt("next"); require(next > offset && next <= 17); page(next)
            }
        }
    }
    when {
        target == "all" -> page()
        Regex("g[1-8]").matches(target) -> request("get_group", JSONObject().put("id", target)) { data ->
            val members = data.getJSONArray("members"); require(members.length() in 1..17)
            for (i in 0 until members.length()) ids.add(members.getString(i))
            read()
        }
        target == "display" || Regex("[0-9a-fA-F]{16}").matches(target) -> { ids.add(target); read() }
        else -> reply(Result.failure(IllegalArgumentException("Vyber světla v nastavení dlaždice.")))
    }
}

/** Bound only while the shade is visible. Showing the shade alone never opens a BLE connection. */
class LightTileService : TileService() {
    private val app get() = application as LampApp
    private val handler = Handler(Looper.getMainLooper())
    private var listening = false
    private var reading = false
    private var nextRead = 0L
    private val tick = object : Runnable {
        override fun run() {
            if (!listening) return
            if (app.client.ready && !app.tileBusy && !reading && SystemClock.elapsedRealtime() >= nextRead) {
                reading = true; nextRead = SystemClock.elapsedRealtime() + 4000
                readTilePower(app.client, app.tilePrefs.getString("target", "all") ?: "all") { result ->
                    reading = false
                    if (!app.tileBusy) result.fold({ app.tileOn = it; app.tileStatus = if (it) "Zapnuto" else "Vypnuto" },
                        { app.tileOn = null; app.tileStatus = it.message ?: "Stav není dostupný" })
                    if (listening) render()
                }
            }
            render(); handler.postDelayed(this, 500)
        }
    }
    override fun onStartListening() {
        super.onStartListening(); listening = true; app.tileListening = true
        handler.removeCallbacks(tick); handler.post(tick)
    }
    override fun onStopListening() {
        listening = false; app.tileListening = false; handler.removeCallbacks(tick)
        app.releaseTileConnection(); app.releaseAlarmConnection(); super.onStopListening()
    }
    private fun render() {
        val tile = qsTile ?: return
        tile.label = "Z-lamp"; tile.icon = Icon.createWithResource(this, R.drawable.ic_tile_light)
        val connected = app.client.ready
        val description = when {
            app.tileBusy -> "Přepínám světla…"
            !ArrivalService.permitted(this) -> "Povol Bluetooth v aplikaci"
            app.client.savedAddress.isEmpty() -> "Nejdřív spáruj lampu"
            !connected -> "Klepni pro připojení"
            else -> app.tileStatus
        }
        tile.state = when {
            app.tileBusy -> Tile.STATE_UNAVAILABLE
            connected && app.tileOn == true -> Tile.STATE_ACTIVE
            else -> Tile.STATE_INACTIVE
        }
        tile.contentDescription = "Z-lamp · $description"
        if (Build.VERSION.SDK_INT >= 29) tile.subtitle = description
        if (Build.VERSION.SDK_INT >= 30) tile.stateDescription = description
        tile.updateTile()
    }
    override fun onClick() {
        super.onClick()
        if (app.tileBusy) return
        // Light control is deliberately available while locked; no activity or unlockAndRun.
        try {
            require(ArrivalService.permitted(this)) { "Povol Bluetooth v aplikaci Z-lamp." }
            require(app.client.bluetoothEnabled) { "Zapni Bluetooth telefonu." }
            require(app.client.savedAddress.isNotEmpty() && app.client.isBonded(app.client.savedAddress)) { "Nejdřív spáruj lampu v aplikaci Z-lamp." }
            app.tileBusy = true
            startForegroundService(Intent(this, TileControlService::class.java))
        } catch (e: RuntimeException) {
            app.tileBusy = false; app.tileStatus = e.message ?: "Otevři aplikaci Z-lamp."
            Toast.makeText(this, app.tileStatus, Toast.LENGTH_LONG).show()
        }
        render()
    }
    override fun onDestroy() { onStopListening(); super.onDestroy() }
}

/** A bounded command survives closing the shade, without enabling the arrival automation. */
class TileControlService : Service() {
    private val app get() = application as LampApp
    private val client get() = app.client
    private val handler = Handler(Looper.getMainLooper())
    private var active = false
    private var reading = false
    private var address = ""
    private var target = "all"
    private var wake: PowerManager.WakeLock? = null
    private val timeout = Runnable {
        if (active) { finish(Result.failure(IllegalStateException("Lampa neodpovídá. Příkaz se nebude opakovat."))); client.disconnect() }
    }
    private val waitForConnection = object : Runnable {
        override fun run() {
            if (!active || reading) return
            if (client.connectionAddress != address) { finish(Result.failure(IllegalStateException("Výběr lampy se změnil."))); return }
            if (client.ready) {
                reading = true; app.arrival.manualControl()
                readTilePower(client, target) { state ->
                    if (!active) return@readTilePower
                    state.fold({ wasOn ->
                        client.control(target, JSONObject().put("on", !wasOn)) { result ->
                            if (active) finish(result.map { !wasOn })
                        }
                    }, { finish(Result.failure(it)) })
                }
            } else if (!client.connecting) finish(Result.failure(IllegalStateException(client.status)))
            else handler.postDelayed(this, 200)
        }
    }
    override fun onBind(intent: Intent?): IBinder? = null
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (active) return START_NOT_STICKY // Double taps never queue two opposing commands.
        if (intent == null) { app.tileBusy = false; stopSelf(); return START_NOT_STICKY }
        active = true; app.tileBusy = true
        try {
            getSystemService(NotificationManager::class.java).createNotificationChannel(
                NotificationChannel("tile", "Ovládání z lišty", NotificationManager.IMPORTANCE_LOW).apply { setShowBadge(false) })
            val open = PendingIntent.getActivity(this, 4, Intent(this, MainActivity::class.java).putExtra("tile", true), PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            startForeground(32, Notification.Builder(this, "tile").setSmallIcon(R.drawable.ic_tile_light).setContentTitle("Z-lamp")
                .setContentText("Přepínám světla…").setContentIntent(open).setOngoing(true).setOnlyAlertOnce(true).build())
            require(ArrivalService.permitted(this) && client.bluetoothEnabled) { "Zapni a povol Bluetooth telefonu." }
            address = client.savedAddress
            require(address.isNotEmpty() && client.isBonded(address)) { "Nejdřív spáruj lampu v aplikaci Z-lamp." }
            require(!client.scanning && !client.pairing) { "Dokonči nejdřív párování v aplikaci." }
            target = app.tilePrefs.getString("target", "all") ?: "all"
            require(!client.connecting || client.connectionAddress == address) { "Probíhá připojování k jiné lampě." }
            wake = getSystemService(PowerManager::class.java).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Zlamp:Tile").apply { acquire(65000) }
            handler.postDelayed(timeout, 60000)
            if (!client.ready && !client.connecting) { app.tileOwnsConnection = true; client.connect(address, autoConnect = true) }
            handler.post(waitForConnection)
        } catch (e: RuntimeException) { finish(Result.failure(e)) }
        return START_NOT_STICKY
    }
    private fun finish(result: Result<Boolean>) {
        if (!active) return
        active = false; app.tileBusy = false; handler.removeCallbacksAndMessages(null)
        result.fold({ app.tileOn = it; app.tileStatus = if (it) "Zapnuto" else "Vypnuto" }, {
            app.tileOn = null; app.tileStatus = it.message ?: "Příkaz nepotvrzen"
            Toast.makeText(this, app.tileStatus, Toast.LENGTH_LONG).show()
        })
        if (wake?.isHeld == true) wake?.release()
        app.releaseTileConnection(); app.releaseAlarmConnection(); stopForeground(STOP_FOREGROUND_REMOVE); stopSelf()
    }
    override fun onDestroy() {
        active = false; handler.removeCallbacksAndMessages(null); app.tileBusy = false
        if (wake?.isHeld == true) wake?.release()
        app.releaseTileConnection(); app.releaseAlarmConnection(); super.onDestroy()
    }
}
