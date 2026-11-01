package cz.zlamp

import org.json.JSONObject
import java.util.ArrayDeque

/** One in flight lives in BleClient. This bounded queue never replays across a connection. */
class Requests {
    data class Job(val op: String, val args: JSONObject, val reply: (Result<JSONObject>) -> Unit)
    private val queue = ArrayDeque<Job>()
    val size get() = queue.size
    fun add(op: String, args: JSONObject, reply: (Result<JSONObject>) -> Unit): Boolean {
        if (queue.size >= 32) return false
        queue.add(Job(op, JSONObject(args.toString()), reply)); return true
    }
    fun control(target: String, patch: JSONObject, reply: (Result<JSONObject>) -> Unit): Boolean {
        // A plain ON must stay separate: firmware uses it to recall the bulb's wake preset.
        val pending = queue.peekLast()?.takeIf { it.op == "control" && it.args.optString("target") == target && !patch.has("on") && !it.args.getJSONObject("patch").has("on") }
        if (pending != null) {
            val merge = pending.args.getJSONObject("patch")
            patch.keys().forEach { merge.put(it, patch.get(it)) }
            return true
        }
        return add("control", JSONObject().put("target", target).put("patch", patch), reply)
    }
    fun take(): Job? = queue.poll()
    fun clear(reason: String) {
        val removed = queue.toList(); queue.clear()
        removed.forEach { it.reply(Result.failure(IllegalStateException(reason))) }
    }
    fun discardControls() { queue.removeIf { it.op == "control" } }
}
