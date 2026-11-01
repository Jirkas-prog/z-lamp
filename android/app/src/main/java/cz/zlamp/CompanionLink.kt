@file:Suppress("DEPRECATION")
package cz.zlamp

import android.annotation.TargetApi
import android.companion.*
import android.content.Context
import android.content.pm.PackageManager
import android.os.*

/** Android observes the approved device; this app still owns its single GATT connection. */
object CompanionLink {
    fun supported(context: Context) = Build.VERSION.SDK_INT >= 31 &&
        context.packageManager.hasSystemFeature(PackageManager.FEATURE_COMPANION_DEVICE_SETUP)
    private fun prefs(context: Context) = context.getSharedPreferences("companion", Context.MODE_PRIVATE)
    fun address(context: Context) = prefs(context).getString("address", "").orEmpty()
    fun enabled(context: Context) = prefs(context).getBoolean("enabled", false)
    fun matches(context: Context, address: String) = enabled(context) && address == address(context) &&
        address == (context.applicationContext as LampApp).client.savedAddress

    @TargetApi(31)
    fun enable(context: Context, address: String) {
        require(supported(context)) { "Systémové připojování vyžaduje Android 12 nebo novější." }
        val app = context.applicationContext as LampApp
        require(address == app.client.savedAddress && app.client.isBonded(address)) { "Nejdřív připoj uloženou lampu." }
        val manager = context.getSystemService(CompanionDeviceManager::class.java)
        require(address in manager.associations) { "Android zatím propojení neschválil." }
        manager.startObservingDevicePresence(address)
        prefs(context).edit().putString("address", address).putBoolean("enabled", true).apply()
    }

    fun disable(context: Context, forget: Boolean = false) {
        val address = address(context)
        prefs(context).edit().putBoolean("enabled", false).remove("pending").apply()
        (context.applicationContext as LampApp).companionPresent = false
        if (Build.VERSION.SDK_INT >= 31 && address.isNotBlank()) {
            val manager = context.getSystemService(CompanionDeviceManager::class.java)
            try { manager?.stopObservingDevicePresence(address) } catch (_: RuntimeException) { }
            if (forget) try { manager?.disassociate(address) } catch (_: RuntimeException) { }
        }
        if (forget) prefs(context).edit().clear().apply()
    }
}

/** Bound and restarted by Android while our approved lamp is nearby. No permanent scan. */
@TargetApi(31)
class LampCompanionService : CompanionDeviceService() {
    private val app get() = application as LampApp
    private val handler = Handler(Looper.getMainLooper())
    private var nearby = ""
    private var retryAt = 0L
    private var delay = 5000L
    private val tick = object : Runnable {
        override fun run() {
            if (!CompanionLink.matches(this@LampCompanionService, nearby)) { release(); return }
            val client = app.client
            if (ArrivalService.permitted(this@LampCompanionService) && !client.bluetoothEnabled && (client.ready || client.connecting))
                client.disconnect("Čekám na zapnutí Bluetooth telefonu.")
            if (!app.choosingLamp && ArrivalService.permitted(this@LampCompanionService) && client.bluetoothEnabled && client.isBonded(nearby)) {
                if (client.ready) delay = 5000
                else if (!client.connecting && !client.scanning && SystemClock.elapsedRealtime() >= retryAt) {
                    retryAt = SystemClock.elapsedRealtime() + delay
                    delay = (delay * 2).coerceAtMost(60000)
                    client.connect(nearby, autoConnect = true)
                }
            }
            handler.postDelayed(this, 2000)
        }
    }
    override fun onDeviceAppeared(address: String) = appeared(address)
    override fun onDeviceDisappeared(address: String) { if (address == nearby) release() }
    @TargetApi(33)
    override fun onDeviceAppeared(associationInfo: AssociationInfo) {
        associationInfo.deviceMacAddress?.toString()?.uppercase()?.let(::appeared)
    }
    @TargetApi(33)
    override fun onDeviceDisappeared(associationInfo: AssociationInfo) {
        associationInfo.deviceMacAddress?.toString()?.uppercase()?.let { onDeviceDisappeared(it) }
    }
    private fun appeared(address: String) {
        if (!CompanionLink.matches(this, address)) return
        nearby = address; app.companionPresent = true
        handler.removeCallbacks(tick); handler.post(tick)
        if (app.arrivalPrefs.getBoolean("enabled", false) && !app.arrivalRunning && ArrivalService.permitted(this)) {
            try { ArrivalService.start(this) } catch (_: RuntimeException) { app.arrivalStatus = "Otevři aplikaci pro obnovení Příchodu." }
        }
    }
    private fun release() {
        handler.removeCallbacks(tick); nearby = ""; app.companionPresent = false
        if (!app.uiVisible && !app.arrivalPrefs.getBoolean("enabled", false) && !app.tileBusy && !app.alarmBusy && !app.tileListening)
            app.client.disconnect()
    }
    override fun onDestroy() { release(); super.onDestroy() }
}
