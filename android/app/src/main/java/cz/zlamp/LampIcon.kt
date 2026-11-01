package cz.zlamp

import android.graphics.*
import android.graphics.drawable.Drawable

/** Small original line icons, drawn on a 24-unit grid. */
class LampIcon(private val kind: String, color: Int) : Drawable() {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        this.color = color; style = Paint.Style.STROKE; strokeWidth = 1.8f
        strokeCap = Paint.Cap.ROUND; strokeJoin = Paint.Join.ROUND
    }
    override fun draw(c: Canvas) {
        c.save(); c.translate(bounds.left.toFloat(), bounds.top.toFloat()); c.scale(bounds.width()/24f, bounds.height()/24f)
        fun line(vararg points: Float) { val p = Path(); p.moveTo(points[0], points[1]); for (i in 2 until points.size step 2) p.lineTo(points[i], points[i+1]); c.drawPath(p, paint) }
        when (kind) {
            "brand" -> { c.drawCircle(12f,12f,10f,paint); line(8f,8f,16f,8f,8f,16f,16f,16f) }
            "bulb" -> { c.drawArc(5f, 2f, 19f, 16f, 145f, 250f, false, paint); line(6.3f,13f,9f,17f,15f,17f,17.7f,13f); line(9f,20f,15f,20f); line(11f,22f,13f,22f) }
            "scenes" -> { line(12f,2f,14.5f,9.5f,22f,12f,14.5f,14.5f,12f,22f,9.5f,14.5f,2f,12f,9.5f,9.5f,12f,2f) }
            "settings" -> { line(4f,6f,20f,6f); line(4f,12f,20f,12f); line(4f,18f,20f,18f); c.drawCircle(9f,6f,2f,paint); c.drawCircle(16f,12f,2f,paint); c.drawCircle(8f,18f,2f,paint) }
            "bluetooth" -> line(6f,6f,18f,17f,12f,22f,12f,2f,18f,7f,6f,18f)
            "power" -> { c.drawArc(4f,4f,20f,20f,-45f,270f,false,paint); line(12f,2f,12f,11f) }
            "chevron" -> line(9f,5f,16f,12f,9f,19f)
            "plus" -> { line(12f,5f,12f,19f); line(5f,12f,19f,12f) }
            "check" -> line(5f,12f,10f,17f,20f,6f)
            "web" -> { c.drawCircle(12f,12f,9f,paint); c.drawOval(8f,3f,16f,21f,paint); line(3f,12f,21f,12f) }
            "play" -> line(8f,4f,20f,12f,8f,20f,8f,4f)
            "stop" -> c.drawRoundRect(5f,5f,19f,19f,3f,3f,paint)
            "heart" -> { val p=Path(); p.moveTo(12f,20f); p.cubicTo(-6f,9f,6f,-3f,12f,6f); p.cubicTo(18f,-3f,30f,9f,12f,20f); c.drawPath(p,paint) }
            "sun" -> { c.drawCircle(12f,12f,4f,paint); for(i in 0..7) { c.save(); c.rotate(i*45f,12f,12f); line(12f,2f,12f,4f); c.restore() } }
        }
        c.restore()
    }
    override fun setAlpha(alpha: Int) { paint.alpha = alpha; invalidateSelf() }
    override fun setColorFilter(filter: ColorFilter?) { paint.colorFilter = filter; invalidateSelf() }
    @Suppress("DEPRECATION") override fun getOpacity() = PixelFormat.TRANSLUCENT
}
