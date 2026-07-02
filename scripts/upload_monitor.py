Import("env")

def upload_and_monitor(source, target, env):
    env.Execute("pio run -e %s -t upload" % env["PIOENV"])
    env.Execute("pio device monitor -e %s" % env["PIOENV"])

# dodaje NOWY przycisk w PlatformIO
env.AddCustomTarget(
    name="upload_monitor",
    dependencies=None,
    actions=[upload_and_monitor],
    title="Upload + Monitor",
    description="Upload firmware and start Serial Monitor"
)
