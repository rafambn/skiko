package org.jetbrains.skia.gpu.graphite

/** Loads the JS re-export module so its native Graphite symbols are installed on window. */
internal actual fun loadGraphiteBindings() {
    check(isSideModuleLoaded()) { "Skiko Graphite JS bindings are not loaded" }
}
