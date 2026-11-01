package cz.zlamp

import org.json.JSONArray
import org.json.JSONObject

/** Explicit offline preview. No request from this mode reaches a radio or a real lamp. */
class DemoLamp {
    val lights = linkedMapOf<String, JSONObject>()
    val groups = linkedMapOf<String, JSONObject>()
    val scenes = linkedMapOf<String, JSONObject>()
    private val timers = linkedMapOf<String, Long>()
    private val remote = JSONObject().put("irEnabled", true).put("irTarget", "all").put("whiteKelvin", 2700).put("whiteLevel", 127)
        .put("brightnessSteps", 10).put("customBrightness", true).put("brightnessLevels", JSONArray(listOf(1,2,4,8,15,25,40,60,80,100)))
        .put("irMembers", JSONArray()).put("buttons", JSONArray(listOf("s1", null, null, null)))
    init {
        listOf("0017880100000001" to "Čtecí lampa", "0017880100000002" to "Police", "display" to "Displej").forEachIndexed { i, (id, name) ->
            val wanted = JSONObject().put("on", i == 0).put("level", 127).put("kelvin", 2700).put("hue", 240).put("saturation", 0)
            lights[id] = JSONObject().put("id", id).put("name", name).put("online", true).put("reported", true).put("on", i == 0).put("level", 127)
                .put("wanted", wanted).put("color", "#ffcc88").put("transition", 3).put("legacyXy", false)
                .put("minKelvin", 2000).put("maxKelvin", 6500).put("manufacturer", "Philips").put("model", "Ukázka")
                .put("startup", JSONObject().put("wakeMode", 0).put("powerMode", 2).put("wake", JSONObject(wanted.toString())).put("power", JSONObject(wanted.toString())))
        }
        groups["g1"] = JSONObject().put("id", "g1").put("name", "Večer").put("members", JSONArray(lights.keys.filter { it != "display" }))
        scenes["s1"] = JSONObject().put("id", "s1").put("name", "Modrá a růžová").put("mode", 12).put("level", 127)
            .put("hold", 0).put("fade", 30).put("useIr", false).put("stagger", false).put("hues", JSONArray(listOf(240, 320)))
            .put("members", JSONArray().put(JSONArray(listOf("0017880100000001", 0))).put(JSONArray(listOf("0017880100000002", 1))))
        scenes["s2"] = JSONObject(scenes.getValue("s1").toString()).put("id", "s2").put("name", "Zlatá hodina")
            .put("level", 95).put("hues", JSONArray(listOf(22, 40, 330)))
        scenes["s3"] = JSONObject(scenes.getValue("s1").toString()).put("id", "s3").put("name", "Noční proud")
            .put("hues", JSONArray(listOf(180, 270, 215)))
    }
    fun handle(op: String, args: JSONObject): JSONObject {
        val map = when {
            op.contains("light") || op in listOf("rename", "device", "startup", "remove") -> lights
            op.contains("group") -> groups
            else -> scenes
        }
        if (op.startsWith("list_")) {
            val offset = args.optInt("offset"); val values = map.values.toList()
            val summaries = values.drop(offset).take(4).map { item -> JSONObject().put("id", item.getString("id")).put("name", item.getString("name")).also { if (op == "list_lights") it.put("online", item.optBoolean("online")) } }
            return JSONObject().put("items", JSONArray(summaries)).put("next", if (offset+4 < values.size) offset+4 else JSONObject.NULL)
        }
        if (op in listOf("get_light", "get_group", "get_scene")) return JSONObject((map[args.getString("id")] ?: error("Položka neexistuje.")).toString())
        when (op) {
            "get_timers" -> return JSONObject().put("items", JSONArray(timers.map { (id, due) -> JSONObject().put("id", id).put("seconds", ((due-System.currentTimeMillis())/1000).coerceAtLeast(0)) }))
            "timer" -> {
                val target = args.getString("target")
                val ids = if (target == "all") lights.keys.filter { it != "display" } else groups[target]?.getJSONArray("members")?.let { a -> List(a.length()) { a.getString(it) } } ?: listOf(target)
                ids.filter { it != "display" }.forEach { id -> if (args.getInt("seconds") == 0) timers.remove(id) else timers[id] = System.currentTimeMillis()+args.getInt("seconds")*1000L }
            }
            "get_remote" -> return JSONObject(remote.toString())
            "remote" -> args.keys().forEach { remote.put(it, args.get(it)) }
            "get_network_status" -> return JSONObject().put("alias", "Ukázková lampa").put("coordinator", true).put("staConnected", false).put("apActive", false)
            "control" -> {
                val target = args.getString("target"); val patch = args.getJSONObject("patch")
                val ids = if (target == "all") lights.keys.filter { it != "display" } else groups[target]?.getJSONArray("members")?.let { a -> List(a.length()) { a.getString(it) } } ?: listOf(target)
                ids.forEach { id -> lights[id]?.let { light -> patch.keys().forEach { light.getJSONObject("wanted").put(it, patch.get(it)) }; light.put("on", light.getJSONObject("wanted").optBoolean("on")); light.put("level", light.getJSONObject("wanted").optInt("level")) } }
            }
            "rename", "device", "startup" -> {
                val light = lights[args.getString("id")] ?: error("Neznámé světlo.")
                val dest = if (op == "startup") light.getJSONObject("startup") else light
                args.keys().forEach { if (it != "id") dest.put(it, args.get(it)) }
            }
            "remove" -> lights.remove(args.getString("id"))
            "group", "scenes" -> {
                val dest = if (op == "group") groups else scenes; val prefix = if (op == "group") "g" else "s"
                if (args.optBoolean("delete")) dest.remove(args.getString("id"))
                else {
                    val id = args.optString("id").ifEmpty { (1..8).map { "$prefix$it" }.firstOrNull { it !in dest } ?: error("Dosažen limit osmi položek.") }
                    dest[id] = JSONObject(args.toString()).put("id", id)
                }
            }
        }
        return JSONObject()
    }
}
