package cz.zlamp

import kotlin.math.pow
import kotlin.math.roundToInt

/** Phone-local clock; the quiet period can cross midnight. No scheduling of existing lights. */
data class ArrivalSchedule(val evening: Int = 21 * 60, val night: Int = 23 * 60,
    val morning: Int = 7 * 60, val dayPercent: Int = 100, val nightPercent: Int = 1) {
    private val dimDuration = (night - evening + 1440) % 1440
    private val quietDuration = (morning - evening + 1440) % 1440
    init {
        require(listOf(evening, night, morning).all { it in 0..1439 } && dimDuration in 1..1438 && quietDuration > dimDuration) {
            "Časy musí jít v pořadí večer → noc → ráno během jednoho dne."
        }
        require(dayPercent in 1..100 && nightPercent in 1..dayPercent) { "Noční jas musí být 1 % až denní jas." }
    }
    data class Plan(val phase: String, val percent: Int) {
        val level get() = (percent * 254.0 / 100).roundToInt().coerceIn(1, 254)
    }
    fun at(minute: Int): Plan {
        require(minute in 0..1439)
        val offset = (minute - evening + 1440) % 1440
        return when {
            offset >= quietDuration -> Plan("day", dayPercent)
            offset >= dimDuration -> Plan("night", nightPercent)
            else -> Plan("evening", (dayPercent * (nightPercent.toDouble() / dayPercent).pow(offset.toDouble() / dimDuration)).roundToInt().coerceIn(nightPercent, dayPercent))
        }
    }
    companion object {
        fun parse(text: String): Int {
            require(Regex("\\d{1,2}:\\d{2}").matches(text.trim())) { "Čas zadej jako HH:mm, například 21:00." }
            val parts = text.trim().split(':').map { it.toInt() }
            require(parts[0] in 0..23 && parts[1] in 0..59) { "Neplatný čas." }
            return parts[0] * 60 + parts[1]
        }
        fun format(minute: Int) = "%02d:%02d".format(minute / 60, minute % 60)
    }
}
