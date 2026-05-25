import argparse
import queue
import socket
import sys
import threading
import time


class ConsoleClient:
    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port
        self.sock: socket.socket | None = None
        self.running = True
        self.monitor_active = False
        self.lines: queue.Queue[str] = queue.Queue()
        self.receiver: threading.Thread | None = None
        self._connect()

    def close(self) -> None:
        self.running = False
        self._disconnect()

    def _connect(self) -> None:
        sock = socket.create_connection((self.host, self.port), timeout=5)
        sock.settimeout(0.5)
        self.sock = sock
        self.receiver = threading.Thread(target=self._recv_loop, args=(sock,), daemon=True)
        self.receiver.start()

    def _disconnect(self) -> None:
        if self.sock is None:
            return
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass
        self.sock = None

    def _recv_loop(self, sock: socket.socket) -> None:
        pending = ""
        try:
            while self.running:
                if self.sock is not sock:
                    break
                try:
                    data = sock.recv(1024)
                except socket.timeout:
                    continue
                if not data:
                    break
                pending += data.decode("utf-8", errors="replace")
                while "\n" in pending:
                    line, pending = pending.split("\n", 1)
                    line = line.rstrip("\r")
                    if self.monitor_active:
                        print(line)
                    else:
                        self.lines.put(line)
        except OSError:
            pass
        finally:
            if self.sock is sock:
                self.sock = None
                self.running = False

    def send_command(self, command: str, idle_timeout: float = 0.25) -> list[str]:
        if self.sock is None:
            raise OSError("Console is disconnected")

        while not self.lines.empty():
            try:
                self.lines.get_nowait()
            except queue.Empty:
                break

        self.sock.sendall((command + "\n").encode("utf-8"))

        response: list[str] = []
        deadline = time.time() + idle_timeout
        while time.time() < deadline:
            try:
                line = self.lines.get(timeout=0.05)
            except queue.Empty:
                continue
            response.append(line)
            deadline = time.time() + idle_timeout
        return [line for line in response if line]

    def print_response(self, lines: list[str]) -> None:
        if not lines:
            print("(no response)")
            return
        for line in lines:
            print(line)

    def drain_lines(self, idle_timeout: float = 0.2) -> list[str]:
        response: list[str] = []
        deadline = time.time() + idle_timeout
        while time.time() < deadline:
            try:
                line = self.lines.get(timeout=0.05)
            except queue.Empty:
                continue
            response.append(line)
            deadline = time.time() + idle_timeout
        return [line for line in response if line]

    def show_menu(self) -> None:
        print("\n===== OTA DEV CONSOLE =====")
        print("[1]: Serial Monitor")
        print("[2]: Device Info")
        print("[3]: Device Settings")
        print("[4]: Restart Device")
        print("[5]: Reset Wi-Fi Credentials")
        print("[Q]: Quit")

    def pause(self) -> None:
        input("\nPress Enter to return to the menu...")

    def run_monitor(self) -> None:
        self.print_response(self.send_command("monitor on"))
        self.monitor_active = True
        print("Serial monitor active. Press Enter to exit.")
        try:
            input()
        finally:
            self.monitor_active = False
            self.print_response(self.send_command("monitor off"))

    def show_info(self) -> None:
        self.print_response(self.send_command("info", idle_timeout=0.75))
        self.pause()

    def show_settings(self) -> None:
        self.print_response(self.send_command("settings", idle_timeout=0.5))
        self.pause()

    def restart_device(self) -> bool:
        self.print_response(self.send_command("restart"))
        self.running = True
        time.sleep(0.5)
        self._disconnect()

        deadline = time.time() + 10.0
        while time.time() < deadline:
            try:
                self._connect()
                self.running = True
                time.sleep(0.2)
                self.drain_lines()
                return True
            except OSError:
                self._disconnect()
                self.running = True
                time.sleep(0.5)

        input("\nDevice did not come back within 10 seconds. Press Enter to quit...")
        return False

    def reset_wifi_credentials(self) -> bool:
        confirm = input("Reset saved Wi-Fi credentials and reboot into provisioning AP? [y/N]: ").strip().lower()
        if confirm not in {"y", "yes"}:
            return True

        self.print_response(self.send_command("wifi_reset"))
        time.sleep(0.5)
        input("\nDevice is rebooting into provisioning AP. Press Enter to quit...")
        return False

    def edit_setting(self, key: str, prompt: str) -> None:
        value = input(f"{prompt}: ").strip()
        if not value:
            return
        self.print_response(self.send_command(f"set {key} {value}"))

    def settings_menu(self) -> None:
        while True:
            print("\n===== DEVICE SETTINGS =====")
            print("[1]: View Current Settings")
            print("[2]: Set Idle Sleep Timeout (ms)")
            print("[3]: Set Abort Blower Duration (ms)")
            print("[4]: Set Temp Fault High (C)")
            print("[5]: Set Temp Fault Low (C)")
            print("[6]: Set Battery Warning (V)")
            print("[7]: Set Battery Fault (V)")
            print("[8]: Set Sound Volume (0-3)")
            print("[9]: Set Button Press Beep (true/false)")
            print("[0]: Back")
            choice = input("> ").strip().lower()

            if choice == "0":
                return
            if choice == "1":
                self.show_settings()
            elif choice == "2":
                self.edit_setting("idle_sleep_timeout_ms", "New idle sleep timeout ms")
            elif choice == "3":
                self.edit_setting("abort_blower_duration_ms", "New abort blower duration ms")
            elif choice == "4":
                self.edit_setting("temp_fault_high_c", "New temp fault high C")
            elif choice == "5":
                self.edit_setting("temp_fault_low_c", "New temp fault low C")
            elif choice == "6":
                self.edit_setting("battery_warning_v", "New battery warning V")
            elif choice == "7":
                self.edit_setting("battery_fault_v", "New battery fault V")
            elif choice == "8":
                self.edit_setting("sound_volume", "New sound volume 0-3")
            elif choice == "9":
                self.edit_setting("button_press_beep_enabled", "Button press beep true/false")
            else:
                print("Unknown selection.")

    def run(self) -> int:
        print(f"Connected to {self.host}:{self.port}")
        time.sleep(0.1)
        welcome = self.drain_lines()
        if welcome:
            self.print_response(welcome)

        while self.running:
            self.show_menu()
            choice = input("> ").strip().lower()

            if choice == "1":
                self.run_monitor()
            elif choice == "2":
                self.show_info()
            elif choice == "3":
                self.settings_menu()
            elif choice == "4":
                if not self.restart_device():
                    return 1
            elif choice == "5":
                if not self.reset_wifi_credentials():
                    return 0
            elif choice in {"q", "quit", "exit"}:
                return 0
            else:
                print("Unknown selection.")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Connect to the EZQ wireless debug console.")
    parser.add_argument("--host", default="ezq-ctlr-b-dev.local", help="Device hostname or IP")
    parser.add_argument("--port", type=int, default=2323, help="Console TCP port")
    args = parser.parse_args()

    client = ConsoleClient(args.host, args.port)
    try:
        return client.run()
    finally:
        client.close()


if __name__ == "__main__":
    raise SystemExit(main())
