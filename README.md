# mbtn

`mbtn` is a Linux tool that maps shell commands to mouse button combinations and wheel movements. It can leave events visible to applications unchanged or block selected clicks and scrolling.

The program reads events from an `evdev` device in `/dev/input`. In blocking mode, it grabs the device and forwards the remaining events through a virtual device created with `uinput`.

## Requirements

- Linux with `evdev` and `/dev/input` support.
- A C compiler, such as GCC or Clang.
- Access to the selected input device. Managing access rules requires `udevadm`, `groupadd`, `usermod`, and `gpasswd`.
- To block events: the `uinput` kernel module and write access to `/dev/uinput`.

## Get the Source

Clone the repository and enter the project directory:

```sh
git clone https://github.com/mtriam/mousebind.git
cd mousebind
```

## Build

Run this from the project directory:

```sh
./build.sh
```

The script compiles `mbtn.c` with `cc` and creates the `mbtn` executable. 

Equivalent command without the script:

```sh
cc -std=c11 -Wall -Wextra -O2 -o mbtn mbtn.c
```

## Quick Start

1. List detected devices and their buttons:

   ```sh
   sudo ./mbtn --list
   ```

2. If the device is not accessible to the current user, grant access to it. Run this command for an interactive device menu:

   ```sh
   sudo ./mbtn --grant
   ```

   Or specify the device path shown by `--list` directly:

   ```sh
   sudo ./mbtn --grant /dev/input/eventN --user "$USER"
   ```

   After adding the user to the `mbtn-input` group, log out and back in.

3. If your configuration will block events with `!` or use `swallow`, grant access to virtual input devices:

   ```sh
   sudo ./mbtn --grant-uinput --user "$USER"
   ```

   This is not needed for configurations that only run commands without blocking or swallowing input.

4. Create a configuration file, for example by copying [`example.conf`](example.conf), and set its device name to exactly match the name shown by `--list`.

5. Run the program with the chosen file:

   ```sh
   ./mbtn example.conf
   ```

The program runs in the foreground, retries the connection if the mouse is disconnected, and automatically reloads the configuration whenever its file changes. It looks for `mouse.conf` by default; that file is not included in the repository, so you will usually need to provide a path explicitly.

## Configuration

Each binding has this format:

```text
[!][MODIFIER+...]TRIGGER = command
```

Example:

```ini
device_name = GYRO Mouse
scroll_delay = 200
wheel_delay = 150

SIDE+WHEEL_UP = wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+
!SIDE+WHEEL_DOWN = wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-
MIDDLE_CLICK = playerctl play-pause
SIDE_TAP = notify-send "SIDE tapped"
```

`device_name` is required and must exactly match the device name. The configuration must contain at least one binding. Button and trigger names are case-insensitive.

### Triggers and Buttons

Use `sudo ./mbtn --list` to see the buttons detected on each device.

- Vertical wheel: `WHEEL_UP`, `WHEEL_DOWN`.
- Horizontal wheel: `WHEEL_LEFT`, `WHEEL_RIGHT`.
- Button press: `<BUTTON>_CLICK`.
- Standalone click on release: `<BUTTON>_TAP`. It does not fire if the button was part of a combination with another button or the wheel.
- Buttons: `LEFT`, `RIGHT`, `MIDDLE`, `SIDE`, `EXTRA`, `FORWARD`, `BACK`, `TASK`, `0`-`9`. The `BTN_` prefix is optional; for example, `BTN_SIDE` and `SIDE` mean the same button.
- Other button codes in the `0x100`-`0x13f` range can be specified as `CODE_<code>`, for example `CODE_0x118`.

Modifiers are joined with `+`, for example `SIDE+EXTRA+WHEEL_UP`. Matching is exact: a binding without modifiers only fires when no tracked button is held. Buttons are tracked if they are used as modifiers, in `_TAP` triggers, or in the `swallow` option.

### Wheel Delays

- `scroll_delay` sets the minimum interval in milliseconds between vertical wheel commands.
- `wheel_delay` sets the same interval for horizontal wheel commands.
- Both options are optional and default to `0` (no limit). Values must be non-negative integers.

### Blocking Events

Events are not blocked by default: clicks still reach applications and the wheel still scrolls. The `!` prefix blocks a trigger, for example:

```ini
!SIDE+WHEEL_UP = wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+
!RIGHT_CLICK =
```

An empty command is allowed only for a blocking binding. A blocked click sends neither the button press nor the release to the application.

Modifiers are still forwarded by themselves. To prevent a modifier click from reaching the application after it is used in a combination, add it to `swallow`:

```ini
swallow = SIDE, EXTRA
SIDE+WHEEL_UP = wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+
```

Buttons listed in `swallow` are forwarded as clicks only after release, provided they were not part of a combination. This can delay the click and prevent dragging with those buttons. `!BUTTON_TAP` is allowed only when that button is listed in `swallow`.

Blocking requires access to `/dev/uinput`. Configure it once with:

```sh
sudo ./mbtn --grant-uinput --user "$USER"
```

After changing membership in the `mbtn-input` group, log out and back in. Access to `uinput` allows creating virtual input devices and injecting events, so grant it only to a trusted user. When `mbtn` exits, the device grab is released.

Commands in the configuration are run in the background via `sh -c`; there is no need to append `&`. A whole command can optionally be wrapped in double quotes. Comments are supported only as separate lines beginning with `#`; trailing comments are not supported. The parser splits each line at the first `=`, so additional `=` characters can be part of the command.

## Commands

```text
./mbtn [config-file] [--replace-existing]
./mbtn --list
./mbtn --monitor [/dev/input/eventN]
sudo ./mbtn --grant [/dev/input/eventN] [--user NAME]
sudo ./mbtn --revoke [/dev/input/eventN | VID:PID] [--user NAME]
sudo ./mbtn --grant-uinput [--user NAME]
sudo ./mbtn --revoke-uinput [--user NAME]
```

- `--list` lists mouse devices, their buttons, wheels, and access status.
- `--monitor` prints detected button presses, releases, and wheel movements in configuration syntax. Without a path, it shows a device menu. Stop it with `Ctrl+C`.
- `--grant` creates a udev rule that gives the specified user access to a mouse. Without a path, it shows a menu.
- `--revoke` removes an access rule; it accepts a device path or a `VID:PID` identifier, for example `32c2:0012`.
- `--grant-uinput` and `--revoke-uinput` enable or disable the user's access to virtual input devices; they are needed only when using `!` or `swallow`.
- `--replace-existing` stops other running instances of the same executable before starting the program.

The `--grant`, `--revoke`, `--grant-uinput`, and `--revoke-uinput` operations require root privileges. If `--user` is omitted, the program uses the user specified by `sudo` (`SUDO_USER`).

## Security

Use these permission commands only for users you trust:

- `--grant` adds the user to the `mbtn-input` group and creates a udev rule granting access to the selected mouse. This lets the user and their processes read events from that device, which may expose input activity. The rule matches the device name and VID:PID, so it can also apply to another connected device with the same identifiers and name.
- `--grant-uinput` grants members of `mbtn-input` write access to `/dev/uinput`. They can create virtual input devices and inject input events, including keyboard events. This is a broader system-wide capability than access to one mouse and should be enabled only when blocking or `swallow` is needed.

Remove permissions you no longer need with `sudo ./mbtn --revoke` or `sudo ./mbtn --revoke-uinput`. The device grab used for blocking is released when `mbtn` exits.
