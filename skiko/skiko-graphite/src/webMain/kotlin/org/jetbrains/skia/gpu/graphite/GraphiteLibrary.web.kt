package org.jetbrains.skia.gpu.graphite

internal expect fun loadGraphiteBindings()

internal actual object GraphiteLibrary {
    actual fun load() = loadGraphiteBindings()
}
