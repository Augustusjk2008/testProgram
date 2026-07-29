"""Generate deterministic, independent HELM performance-analysis fixtures.

This script deliberately uses only Python's standard library and closed-form
signal models.  It neither invokes nor imports the C++ analyzer under test.
Run from this directory with: python generate_fixtures.py
"""

import hashlib
import json
import math
from pathlib import Path


ROOT = Path(__file__).resolve().parent
PI = math.pi
GENERATOR_VERSION = "1"
ANALYZER_VERSION = (
    "mbddf.helm.performance/1;cyclesPerEstimate=4;maxDelayMs=100;"
    "frequencyPointCount=96;candidateSamples=2048;"
    "candidateSampling=time_stratified;"
    "minCommandCorrelation=0.95;absoluteExcitationFloor=1e-6;"
    "relativeExcitationRatio=0.01"
)


def parameters(waveform, freq=1.0, ampl=1.0, max_freq=8.0, duration=8.0):
    return {
        "waveform": waveform,
        "freq": freq,
        "ampl": ampl,
        "offset": 0.0,
        "start": 0.0,
        "max_freq": max_freq,
        "sweep_duration_s": duration,
        "enable": 1,
    }


def sample(t_us, command, feedback, sequence):
    return {
        "tUs": int(t_us),
        "command": [command, command, command, command],
        "feedback": [feedback, feedback, feedback, feedback],
        "status": 0,
        "errCode": 0,
        "selfCheck": 0,
        "timeout": 0,
        "productSequence": sequence & 0xFFFF,
        "serialA": sequence & 0xFFFF,
        "serialB": sequence & 0xFFFF,
        "ddsSequence": int(t_us + 1000000),
        "batchIndex": 0,
    }


def write_fixture(name, params, samples, expected, coverage):
    document = {
        "fixtureSchemaVersion": "1",
        "analyzerVersion": ANALYZER_VERSION,
        "name": name,
        "parameters": params,
        "samples": samples,
        "expected": expected,
        "generation": {
            "method": "closed-form signal models / explicit Euler first-order plant",
            "generator": "generate_fixtures.py",
            "generatorVersion": GENERATOR_VERSION,
            "tolerance": "See per-test assertions; values do not call the C++ analyzer.",
            "coverage": coverage,
        },
    }
    path = ROOT / f"{name}.json"
    path.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return path


def constant_fixture():
    samples = []
    for index in range(151):
        t = index * 0.01
        feedback = 0.1 + 0.02 * math.sin(2.0 * PI * 10.0 * t)
        samples.append(sample(round(t * 1_000_000), 0.0, feedback, index))
    # Calculate the expected value from the same discrete tail interval that
    # the specification defines, rather than from a continuous approximation.
    duration = samples[-1]["tUs"] / 1_000_000.0 - samples[0]["tUs"] / 1_000_000.0
    window = min(duration, max(0.2 * duration, 0.2))
    tail = [entry["feedback"][0] for entry in samples
            if entry["tUs"] / 1_000_000.0 >= duration - window]
    tail_mean = sum(tail) / len(tail)
    tail_stddev = math.sqrt(sum((value - tail_mean) ** 2 for value in tail) / len(tail))
    return write_fixture(
        "constant",
        parameters(3, ampl=5.0),
        samples,
        {"steadyMeanError": tail_mean, "steadyStddev": tail_stddev},
        ["constant", "zero-target", "tail-window", "non-dynamic-excitation-floor"],
    )


def sine_fixture():
    samples = []
    t_us = 0
    index = 0
    frequency = 2.0
    amplitude = 2.0
    feedback_amplitude = 1.5
    lag = PI / 5.0
    while t_us <= 3_100_000:
        t = t_us / 1_000_000.0
        command = amplitude * math.sin(2.0 * PI * frequency * t)
        feedback = feedback_amplitude * math.sin(2.0 * PI * frequency * t - lag)
        samples.append(sample(t_us, command, feedback, index))
        t_us += (4500, 5000, 5500, 5000)[index % 4]
        index += 1
    return write_fixture(
        "sine",
        parameters(0, freq=frequency, ampl=amplitude),
        samples,
        {"phaseLagDeg": 36.0, "principalDelayMs": 50.0, "amplitudeRatio": 0.75},
        ["sine", "nonuniform-dds-time", "positive-phase-lag", "three-stable-cycles"],
    )


def square_command(t):
    return -1.0 if int(math.floor(t / 0.5)) % 2 == 0 else 1.0


def square_fixture():
    samples = []
    previous = square_command(0.0)
    target = previous
    edge_time = 0.0
    for index in range(401):
        t = index * 0.01
        command = square_command(t)
        if command != previous:
            target = command
            edge_time = t
            previous = command
        before = -target
        elapsed = t - edge_time
        if elapsed <= 0.05:
            feedback = before
        elif elapsed < 0.15:
            feedback = before + (target - before) * ((elapsed - 0.05) / 0.10)
        else:
            feedback = target
        samples.append(sample(round(t * 1_000_000), command, feedback, index))
    return write_fixture(
        "square",
        parameters(1, freq=1.0, ampl=1.0),
        samples,
        {"delayMs": 60.0},
        ["square", "rising-edge", "falling-edge", "linear-crossing", "complete-platforms"],
    )


def triangle_command(t):
    phase = t % 1.0
    if phase < 0.5:
        return -1.0 + 4.0 * phase
    return 3.0 - 4.0 * phase


def triangle_fixture():
    samples = []
    last_command = triangle_command(0.0)
    for index in range(401):
        t = index * 0.01
        command = triangle_command(t)
        direction = 1.0 if command >= last_command else -1.0
        feedback = 0.8 * command + 0.05 * direction
        samples.append(sample(round(t * 1_000_000), command, feedback, index))
        last_command = command
    return write_fixture(
        "triangle",
        parameters(2, freq=1.0, ampl=1.0),
        samples,
        {"slopeRatio": 0.8},
        ["triangle", "turning-dead-zone", "directional-tracking", "nonuniform-direction"],
    )


def sweep_phase(tau, f0, f1, duration):
    if f0 == f1:
        return 2.0 * PI * f0 * tau
    return (2.0 * PI * f0 * f1 * duration / (f1 - f0)) * math.log(
        (f1 * duration) / (f1 * duration - tau * (f1 - f0))
    )


def sweep_fixture(name, delay_s, tail_s, reverse=False):
    f0, f1 = (8.0, 1.0) if reverse else (1.0, 8.0)
    duration = 8.0
    start_s = 0.1
    sample_dt = 0.002
    total_s = start_s + duration + tail_s
    plant_tau = 1.0 / (2.0 * PI * 2.0)
    delay_count = int(round(delay_s / sample_dt))
    input_history = []
    feedback = 0.0
    samples = []
    count = int(round(total_s / sample_dt)) + 1
    for index in range(count):
        t = index * sample_dt
        tau = t - start_s
        command = math.sin(sweep_phase(tau, f0, f1, duration)) if 0.0 <= tau <= duration else 0.0
        input_history.append(command)
        delayed = input_history[index - delay_count] if index >= delay_count else 0.0
        feedback += sample_dt * (delayed - feedback) / plant_tau
        samples.append(sample(round(t * 1_000_000), command, feedback, index))
    expected = {"direction": "reverse" if reverse else "forward", "delayMs": delay_s * 1000.0}
    return write_fixture(
        name,
        parameters(4, freq=f0, ampl=1.0, max_freq=f1, duration=duration),
        samples,
        expected,
        ["sweep", "first-order-low-pass", "local-synchronous-estimate",
         "reverse" if reverse else "forward", "truncated-tail" if tail_s < 0.1 else "complete-tail"],
    )


def second_order_sweep_fixture():
    f0, f1 = 1.0, 8.0
    duration = 8.0
    start_s = 0.1
    sample_dt = 0.002
    natural_hz = 2.7
    damping = 0.28
    omega = 2.0 * PI * natural_hz
    velocity = 0.0
    feedback = 0.0
    samples = []
    count = int(round((start_s + duration + 0.20) / sample_dt)) + 1
    for index in range(count):
        t = index * sample_dt
        tau = t - start_s
        command = math.sin(sweep_phase(tau, f0, f1, duration)) if 0.0 <= tau <= duration else 0.0
        acceleration = omega * omega * (command - feedback) - 2.0 * damping * omega * velocity
        velocity += sample_dt * acceleration
        feedback += sample_dt * velocity
        samples.append(sample(round(t * 1_000_000), command, feedback, index))
    return write_fixture(
        "sweep_second_order",
        parameters(4, freq=f0, ampl=1.0, max_freq=f1, duration=duration),
        samples,
        {"naturalFrequencyHz": natural_hz, "damping": damping},
        ["sweep", "second-order-underdamped", "resonance", "complete-tail"],
    )


def pure_delay_sweep_fixture():
    f0, f1 = 1.0, 8.0
    duration = 8.0
    start_s = 0.1
    sample_dt = 0.002
    delay_s = 0.04
    delay_count = int(round(delay_s / sample_dt))
    history = []
    samples = []
    count = int(round((start_s + duration + 0.20) / sample_dt)) + 1
    for index in range(count):
        t = index * sample_dt
        tau = t - start_s
        command = math.sin(sweep_phase(tau, f0, f1, duration)) if 0.0 <= tau <= duration else 0.0
        history.append(command)
        feedback = history[index - delay_count] if index >= delay_count else 0.0
        samples.append(sample(round(t * 1_000_000), command, feedback, index))
    return write_fixture(
        "sweep_pure_delay",
        parameters(4, freq=f0, ampl=1.0, max_freq=f1, duration=duration),
        samples,
        {"delayMs": delay_s * 1000.0},
        ["sweep", "pure-delay", "phase", "complete-tail"],
    )


def early_stop_sweep_fixture():
    f0, f1 = 1.0, 8.0
    duration = 8.0
    start_s = 0.1
    observed_s = 4.6
    sample_dt = 0.002
    plant_tau = 1.0 / (2.0 * PI * 2.0)
    feedback = 0.0
    samples = []
    count = int(round(observed_s / sample_dt)) + 1
    for index in range(count):
        t = index * sample_dt
        tau = t - start_s
        command = math.sin(sweep_phase(tau, f0, f1, duration)) if 0.0 <= tau <= duration else 0.0
        feedback += sample_dt * (command - feedback) / plant_tau
        samples.append(sample(round(t * 1_000_000), command, feedback, index))
    return write_fixture(
        "sweep_early_stop",
        parameters(4, freq=f0, ampl=1.0, max_freq=f1, duration=duration),
        samples,
        {"observedSweepSeconds": observed_s - start_s},
        ["sweep", "forward", "early-stop", "partial-coverage"],
    )


def sequence_fixture(name, gaps):
    samples = []
    sequence = 0
    for index in range(151):
        t = index * 0.01
        feedback = 0.1 + 0.02 * math.sin(2.0 * PI * 10.0 * t)
        if gaps and index in (30, 75, 110):
            sequence += 4
        samples.append(sample(round(t * 1_000_000), 0.0, feedback, sequence))
        sequence += 1
    return write_fixture(
        name,
        parameters(3, ampl=5.0),
        samples,
        {},
        ["sequence-diagnostics", "numerical-invariance", "constant"],
    )


def main():
    files = [
        constant_fixture(),
        sine_fixture(),
        square_fixture(),
        triangle_fixture(),
        sweep_fixture("sweep_first_order", 0.02, 0.20),
        sweep_fixture("sweep_delay_truncated", 0.08, 0.02),
        sweep_fixture("sweep_reverse_complete", 0.02, 0.20, True),
        sweep_fixture("sweep_reverse_truncated", 0.08, 0.02, True),
        second_order_sweep_fixture(),
        pure_delay_sweep_fixture(),
        early_stop_sweep_fixture(),
        sequence_fixture("sequence_diagnostic_a", False),
        sequence_fixture("sequence_diagnostic_b", True),
    ]
    manifest = {
        "fixtureSchemaVersion": "1",
        "analyzerVersion": ANALYZER_VERSION,
        "generator": {
            "path": "generate_fixtures.py",
            "version": GENERATOR_VERSION,
            "sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "method": "independent standard-library closed-form and explicit-Euler models",
        },
        "files": [
            {
                "name": path.stem,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "method": "independently generated; never calls the C++ analyzer",
                "parameters": json.loads(path.read_text(encoding="utf-8"))["parameters"],
                "coverage": json.loads(path.read_text(encoding="utf-8"))["generation"]["coverage"],
                "tolerance": json.loads(path.read_text(encoding="utf-8"))["generation"]["tolerance"],
            }
            for path in files
        ],
    }
    (ROOT / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
