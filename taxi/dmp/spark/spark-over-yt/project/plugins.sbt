addSbtPlugin("com.eed3si9n" % "sbt-assembly" % "0.14.10")

addSbtPlugin("net.virtual-void" % "sbt-dependency-graph" % "0.10.0-RC1")

addSbtPlugin("com.typesafe.sbt" % "sbt-native-packager" % "1.4.1")
libraryDependencies += "org.vafer" % "jdeb" % "1.3" artifacts (Artifact("jdeb", "jar", "jar"))

libraryDependencies += "com.github.eikek" %% "yamusca-core" % "0.5.1"

val circeVersion = "0.12.3"

libraryDependencies ++= Seq(
  "io.circe" %% "circe-core",
  "io.circe" %% "circe-generic",
  "io.circe" %% "circe-parser"
).map(_ % circeVersion)

resolvers += Resolver.url("SparkSnapshots", url("http://artifactory.yandex.net/artifactory/yandex_spark_snapshots"))(
    Patterns()
      .withIvyPatterns(Vector("[organisation]/[module]_2.12_1.0/[revision]/[artifact]-[revision].[ext]"))
      .withArtifactPatterns(Vector("[organisation]/[module]_2.12_1.0/[revision]/[artifact]-[revision].[ext]"))
  )

resolvers += Resolver.url("SparkReleases", url("http://artifactory.yandex.net/artifactory/yandex_spark_releases"))(
    Patterns()
      .withIvyPatterns(Vector("[organisation]/[module]_2.12_1.0/[revision]/[artifact]-[revision].[ext]"))
      .withArtifactPatterns(Vector("[organisation]/[module]_2.12_1.0/[revision]/[artifact]-[revision].[ext]"))
  )

addSbtPlugin("ru.yandex" % "sbt-yandex" % "0.0.3-SNAPSHOT")
