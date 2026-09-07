# Run a Python file inside the RUNNING Unreal editor via PythonScriptPlugin's
# remote-execution protocol (UDP multicast discovery -> TCP command channel).
# Requires: Project Settings > Plugins > Python > Enable Remote Execution.
#
# Usage: python uepy.py <script.py>
import sys, os, time

UE = os.environ.get("UE_ROOT", r"C:\Program Files\Epic Games\UE_5.8")
sys.path.append(os.path.join(UE, "Engine", "Plugins", "Experimental",
                             "PythonScriptPlugin", "Content", "Python"))
import remote_execution as re_mod


def main(path):
    with open(path, "r", encoding="utf-8") as f:
        source = f.read()

    rex = re_mod.RemoteExecution()
    rex.start()
    try:
        # Discovery is multicast + asynchronous. 10s is not enough: a busy editor
        # (user scrubbing, compiling, dialog open) can take well over that to
        # answer, and a short window looks exactly like "server is down".
        node = None
        for _ in range(150):
            if rex.remote_nodes:
                node = rex.remote_nodes[0]
                break
            time.sleep(0.2)

        if node is None:
            print("NO EDITOR FOUND on 239.0.0.1:6766.")
            print("Tick Project Settings > Plugins > Python > Enable Remote Execution.")
            return 2

        print("node: %s" % node.get("node_id"))
        rex.open_command_connection(node)

        res = rex.run_command(source, unattended=True,
                              exec_mode=re_mod.MODE_EXEC_FILE)

        for entry in res.get("output") or []:
            print("[%s] %s" % (entry.get("type"), entry.get("output")))
        if res.get("result"):
            print("result: %s" % res["result"])
        ok = res.get("success")
        print("success: %s" % ok)
        return 0 if ok else 1
    finally:
        rex.stop()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
