package cz.zlamp

import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

object Wire {
    const val MAX = 1536
    fun frames(text: String, mtu: Int): List<ByteArray> {
        val bytes = text.toByteArray(Charsets.UTF_8)
        require(bytes.size in 1..MAX && mtu >= 23) { "Zpráva je příliš velká." }
        val payload = minOf(mtu, 247) - 7
        return (bytes.indices step payload).map { offset ->
            val n = minOf(payload, bytes.size - offset)
            byteArrayOf(bytes.size.toByte(), (bytes.size shr 8).toByte(), offset.toByte(), (offset shr 8).toByte()) + bytes.copyOfRange(offset, offset+n)
        }
    }
    class Receiver {
        private val buffer = ByteArray(MAX)
        private var total = 0
        private var received = 0
        private var last = 0L
        fun reset() { total = 0; received = 0 }
        fun feed(frame: ByteArray, now: Long): String? {
            try {
                require(frame.size >= 5 && (received == 0 || now-last < 5000)) { "Neúplná BLE zpráva." }
                val size = (frame[0].toInt() and 255) or ((frame[1].toInt() and 255) shl 8)
                val offset = (frame[2].toInt() and 255) or ((frame[3].toInt() and 255) shl 8)
                val n = frame.size - 4
                require(size in 1..MAX && offset == received && offset+n <= size && (offset == 0 || total == size)) { "Neplatný BLE fragment." }
                require((4 until frame.size).none { frame[it] == 0.toByte() }) { "Neplatná BLE zpráva." }
                total = size; frame.copyInto(buffer, offset, 4); received += n; last = now
                if (received != total) return null
                val value = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(buffer, 0, total)).toString()
                reset(); return value
            } catch (e: Exception) { reset(); throw e }
        }
    }
}
