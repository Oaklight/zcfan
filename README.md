# zcfan

Fork of [cdown/zcfan](https://github.com/cdown/zcfan) with multi-tier fan
control and ramp-up delay for Meteor Lake ThinkPads.

## What's different from upstream

- **Multi-tier fan control**: configure up to 8 fan tiers via the `tier`
  keyword instead of the fixed 3-tier (max/med/low) setup
- **Ramp-up delay**: configurable `up_delay_ticks` (default 3) delays fan
  speed increases to filter transient temperature spikes
  (cherry-picked from [y1lichen/zcfan](https://github.com/y1lichen/zcfan),
  see [zcfan#40](https://github.com/cdown/zcfan/issues/40))
- **Full backward compatibility**: without `tier` lines, behaves identically
  to upstream (legacy `max_temp`/`med_temp`/`low_temp` keys still work)

## Features

- Extremely small, simple, and easy to understand code
- Sensible out of the box, configuration is optional
- Strong focus on stopping the fan as soon as safe to do so, without inducing
  throttling
- Automatic temperature- and time-based hysteresis: no bouncing between fan
  levels
- Watchdog support
- Minimal resource usage
- No dependencies

## Usage

zcfan reads all temperature sensors present on the system. By default, it has
the following fan states:

| Config name | thinkpad_acpi fan level           | Default trip temperature (C) |
|-------------|-----------------------------------|------------------------------|
| max_temp    | full-speed (or 7 if unsupported)  | 90                           |
| med_temp    | 4                                 | 80                           |
| low_temp    | 1                                 | 70                           |

If no trip temperature is reached, the fan will be turned off.

### Multi-tier configuration

Instead of the 3 fixed tiers, you can define up to 8 custom tiers using the
`tier` keyword. Each line specifies a temperature threshold and a fan level
(0-7, `full-speed`, or `disengaged`):

    tier 45 1
    tier 60 4
    tier 75 5
    tier 90 7

Tiers are automatically sorted by temperature. Below the lowest tier's
threshold, the fan is turned off.

When any `tier` line is present, legacy `max_temp`/`med_temp`/`low_temp` and
`max_level`/`med_level`/`low_level` keys are ignored.

### Ramp-up delay

To prevent fan flutter from brief temperature spikes (common on Intel Meteor
Lake), the fan waits `up_delay_ticks` consecutive seconds above a threshold
before actually ramping up. Default is 3. Set to 0 to disable:

    up_delay_ticks 3

### Legacy configuration

To override the 3 default tiers without using the `tier` keyword, place a file
at `/etc/zcfan.conf` with updated trip temperatures and/or fan levels:

    max_temp 85
    med_temp 70
    low_temp 55
    temp_hysteresis 20

    max_level full-speed
    med_level 4
    low_level 1

### Ignoring sensors

If you have a faulty sensor, or a sensor that you otherwise want to ignore, you
can ignore it using the `ignore_sensor` directive in the config file. For
example:

    % grep . /sys/class/hwmon/*/name
    /sys/class/hwmon/hwmon0/name:AC
    /sys/class/hwmon/hwmon1/name:acpitz
    /sys/class/hwmon/hwmon2/name:BAT0
    /sys/class/hwmon/hwmon3/name:nvme
    /sys/class/hwmon/hwmon4/name:coretemp

If you wanted to ignore the nvme and BAT0 monitors, you'd add to your config
file:

    ignore_sensor nvme
    ignore_sensor BAT0

### Hysteresis

We will only reduce the fan level again once:

1. The temperature is now at least `temp_hysteresis` Celsius (default 10C)
   below the trip point, and
2. At least 3 seconds have elapsed since the initial trip.

This avoids unnecessary fluctuations in fan speed.

## Comparison with thinkfan

I wrote zcfan because I found thinkfan's configuration and code complexity too
much for my tastes. Use whichever suits your needs.

## Compilation

Run `make`.

## Installation

1. Compile zcfan or install from the [AUR
   package](https://aur.archlinux.org/packages/zcfan)
2. Load your thinkpad_acpi module with `fan_control=1`
    - At runtime: `rmmod thinkpad_acpi && modprobe thinkpad_acpi fan_control=1`
    - By default: `echo options thinkpad_acpi fan_control=1 > /etc/modprobe.d/99-fancontrol.conf`
3. Run `zcfan` as root (or use the `zcfan` systemd service provided)
4. If you run a laptop which keeps the fan running during suspend, you will also
   want to send `SIGPWR` before sleep and `SIGUSR2` before wakeup to avoid
   that. For systemd users, enable the `zcfan-sleep.service` and
   `zcfan-resume.service` units to do that automatically.

## Disclaimer

While the author uses this on their own machine, obviously there is no warranty
whatsoever.
