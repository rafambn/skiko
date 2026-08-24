import org.gradle.api.DefaultTask
import org.gradle.api.file.ConfigurableFileCollection
import org.gradle.api.file.DirectoryProperty
import org.gradle.api.file.FileSystemOperations
import org.gradle.api.file.RelativePath
import org.gradle.api.tasks.Classpath
import org.gradle.api.tasks.OutputDirectory
import org.gradle.api.tasks.TaskAction
import javax.inject.Inject

abstract class StageAndroidJniLibsTask @Inject constructor(
    private val fileSystemOperations: FileSystemOperations,
) : DefaultTask() {
    @get:Classpath
    abstract val arm64Archives: ConfigurableFileCollection

    @get:Classpath
    abstract val x64Archives: ConfigurableFileCollection

    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty

    @TaskAction
    fun stage() {
        fileSystemOperations.sync {
            into(outputDirectory)
            from(arm64Archives.files.map(project::zipTree)) {
                include("**/*.so")
                eachFile { relativePath = RelativePath(true, "arm64-v8a", name) }
                includeEmptyDirs = false
            }
            from(x64Archives.files.map(project::zipTree)) {
                include("**/*.so")
                eachFile { relativePath = RelativePath(true, "x86_64", name) }
                includeEmptyDirs = false
            }
        }
    }
}
