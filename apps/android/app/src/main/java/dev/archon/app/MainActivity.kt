package dev.archon.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import dev.archon.app.ui.ArchonApp
import dev.archon.app.ui.ArchonTheme
import dev.archon.app.ui.ArchonViewModel

class MainActivity : ComponentActivity() {
    private val vm: ArchonViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent { ArchonTheme { ArchonApp(vm) } }
    }

    override fun onStart() {
        super.onStart()
        vm.onForeground()
    }

    override fun onStop() {
        vm.onBackground()
        super.onStop()
    }
}
