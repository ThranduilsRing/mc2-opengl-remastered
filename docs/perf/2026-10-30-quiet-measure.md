# Robust paired CPU measurements, 2026-10-30

Engine refs: W86 `c7ef9a666`, main `586327c79`, drawscreen `b409855e4`. The drawscreen executables are rebuilt at tooling-only descendants (host `407088bfc`, laptop stock `ac8486cfc`, laptop battle `409815680`); their engine sources and shaders are unchanged from b409855e4.

Every workload runs five adjacent A/B pairs, in A/B/A/B order, with 20 seconds of gameplay. The first three seconds of Frame events are discarded. Camera poses are fixed across both arms: mc2_01 and mc2_10 sun_facing_low, mc2_24 low_horizon_0; mc2_03 uses the mission view. These are selected heavy views from the earlier sweep, not a new exhaustive camera envelope. Battle adds 128 cloned mechs and two cloned vehicles by main-thread UI commands to mc2_24's 70 stock movers, requires 200 in every world sample, and keeps default simulation overlap. Counts include dead movers.

Timings are elapsed inclusive CPU-zone times, including waits. A mean is the median of capture total scope time divided by Frame count; p95 is the median of capture event p95s, not a pooled event percentile. Min is the median of capture minima and Q1 is the median of capture lower quartiles. For Frame and DrawScreen there is one event per frame; GameLogic can run multiple events per Frame, so its mean/frame can exceed its event p95. Nested scopes must not be summed.

Delta is the median of paired B-minus-A capture medians. The 90% percentile bootstrap resamples whole pairs (10,000 resamples, fixed seed), never individual correlated frame events. Negative CI = WIN; positive CI = REGRESSION. A CI inside +/-0.02 ms containing zero = NO CHANGE; wider intervals containing zero or fewer than five successful pairs = INCONCLUSIVE. Missing/crashed captures stay errors; noise never excludes a capture.

Host games use reserved logical CPUs 8,9,12,13 (0x3300) and exclusive per-capture leases; tools use Idle priority. Both machines record external reserved-core CPU beside timings. Laptop absolute milliseconds describe its own CPU/GPU and are not interchangeable with host milliseconds. Concurrent host services remain present. Load p95 covers the game-process lifetime, including loading; timed scopes use the live Frame window.

| Comparison | Arm | Executable SHA256 |
|---|---|---|
| Host W86/main | A | de8241d4a525ee933a32a618cf304b7a35553d0b4cd0a9e3ea5a2065e499f236 |
| Host W86/main | B | 84c45090b8a4bb288f9644e7bf0c5caf208de3b10949d3ae95f42ec17156f023 |
| Host W86/main battle | B | cdf1de494d023001a8bba51ad4083b95cc90f7d4f3d2193c483fadcd6de76d8b |
| Host main/drawscreen | A | cdf1de494d023001a8bba51ad4083b95cc90f7d4f3d2193c483fadcd6de76d8b |
| Host main/drawscreen | B | f04e16d7a11ffa50d34976d3b4855a5fa35e68b06f5e13a2d23aaeb4f3a80a43 |
| Laptop W86/main | A | de8241d4a525ee933a32a618cf304b7a35553d0b4cd0a9e3ea5a2065e499f236 |
| Laptop W86/main | B | 84c45090b8a4bb288f9644e7bf0c5caf208de3b10949d3ae95f42ec17156f023 |
| Laptop main/drawscreen | A | 84c45090b8a4bb288f9644e7bf0c5caf208de3b10949d3ae95f42ec17156f023 |
| Laptop main/drawscreen | B | c7ab0a0e80ff320de9c8de70fdef4d12017dd7493106eb00b1515ca6c5f11b25 |
| Laptop main/drawscreen battle | B | 00dfa9980340e3e1c693059c021397a71e1329aa48ef19be909b3df7c3a8b668 |

GPU receipts: Host W86/main: AMD Radeon RX 7900 XTX; Host main/drawscreen: AMD Radeon RX 7900 XTX; Laptop W86/main: NVIDIA GeForce GTX 1050 Ti; Laptop main/drawscreen: NVIDIA GeForce GTX 1050 Ti. All successful captures have clean expected build fingerprints, 1920x1080 receipts and identical render-option sidecars.

## Headline Frame / DrawScreen mean ms

Each cell is A -> B, Frame / DrawScreen. Full event p95, paired CI and noise follow.

| Mission | Host W86/main | Host main/drawscreen | Laptop W86/main | Laptop main/drawscreen |
|---|---|---|---|---|
| mc2_01 | 1.290/0.587 -> 1.309/0.616 | 1.336/0.628 -> 1.364/0.643 | 5.834/2.067 -> 5.936/2.259 | 5.977/2.245 -> 5.859/2.195 |
| mc2_03 | 1.469/0.729 -> 1.458/0.746 | 1.516/0.774 -> 1.507/0.773 | 10.620/3.217 -> 10.567/3.152 | 10.519/3.131 -> 10.522/3.123 |
| mc2_10 | 2.237/1.067 -> 2.259/1.095 | 3.018/1.333 -> 3.623/1.259 | 20.306/6.751 -> 18.913/6.582 | 18.839/6.573 -> 19.000/6.581 |
| mc2_24 | 3.974/1.846 -> 4.017/1.887 | 6.712/3.183 -> 6.135/2.901 | 27.929/7.299 -> 27.776/10.986 | 27.794/11.137 -> 27.676/11.046 |
| battle | 12.496/2.571 -> 11.690/2.761 | 13.696/3.193 -> 12.937/3.052 | 41.954/6.573 -> 41.064/6.896 | 37.591/6.540 -> 35.579/6.455 |

## Host W86/main

| Mission | Scope | A mean/frame / event p95 / min / Q1 ms | B mean/frame / event p95 / min / Q1 ms | Paired event-median delta [90% CI] ms | N / verdict | Reserved external CPU p95 A / B % |
|---|---|---:|---:|---:|---|---|
| mc2_01 | Frame | 1.290 / 1.536 / 1.010 / 1.163 | 1.309 / 1.499 / 0.818 / 1.239 | +0.034 [+0.020, +0.047] | 5 / REGRESSION | 5.8-40.7 / 2.9-26.7 |
| mc2_01 | GameLogic | 0.435 / 0.272 / 0.171 / 0.186 | 0.483 / 0.285 / 0.178 / 0.209 | +0.050 [+0.018, +0.054] | 5 / REGRESSION | 5.8-40.7 / 2.9-26.7 |
| mc2_01 | DrawScreen | 0.587 / 0.671 / 0.485 / 0.551 | 0.616 / 0.688 / 0.517 / 0.593 | +0.029 [+0.021, +0.039] | 5 / REGRESSION | 5.8-40.7 / 2.9-26.7 |
| mc2_01 | SwapWindow | 0.184 / 0.254 / 0.132 / 0.147 | 0.005 / 0.006 / 0.003 / 0.005 | -0.152 [-0.160, -0.151] | 5 / WIN | 5.8-40.7 / 2.9-26.7 |
| mc2_03 | Frame | 1.469 / 1.843 / 1.188 / 1.329 | 1.458 / 1.759 / 1.196 / 1.325 | +0.019 [-0.025, +0.076] | 5 / INCONCLUSIVE | 4.0-7.1 / 3.0-9.7 |
| mc2_03 | GameLogic | 0.535 / 0.302 / 0.240 / 0.255 | 0.575 / 0.320 / 0.250 / 0.274 | +0.022 [+0.016, +0.032] | 5 / REGRESSION | 4.0-7.1 / 3.0-9.7 |
| mc2_03 | DrawScreen | 0.729 / 0.885 / 0.625 / 0.679 | 0.746 / 0.917 / 0.648 / 0.699 | +0.020 [+0.014, +0.044] | 5 / REGRESSION | 4.0-7.1 / 3.0-9.7 |
| mc2_03 | SwapWindow | 0.178 / 0.189 / 0.132 / 0.142 | 0.006 / 0.006 / 0.003 / 0.005 | -0.140 [-0.144, -0.138] | 5 / WIN | 4.0-7.1 / 3.0-9.7 |
| mc2_10 | Frame | 2.237 / 4.608 / 1.373 / 1.532 | 2.259 / 5.162 / 1.195 / 1.549 | -0.002 [-0.188, +0.059] | 5 / INCONCLUSIVE | 3.9-12.0 / 2.4-16.3 |
| mc2_10 | GameLogic | 0.933 / 0.828 / 0.305 / 0.323 | 0.962 / 0.833 / 0.319 / 0.344 | +0.022 [-0.001, +0.037] | 5 / INCONCLUSIVE | 3.9-12.0 / 2.4-16.3 |
| mc2_10 | DrawScreen | 1.067 / 2.487 / 0.657 / 0.697 | 1.095 / 2.225 / 0.677 / 0.721 | +0.022 [-0.067, +0.056] | 5 / INCONCLUSIVE | 3.9-12.0 / 2.4-16.3 |
| mc2_10 | SwapWindow | 0.266 / 0.940 / 0.134 / 0.146 | 0.006 / 0.006 / 0.003 / 0.005 | -0.146 [-0.146, -0.144] | 5 / WIN | 3.9-12.0 / 2.4-16.3 |
| mc2_24 | Frame | 3.974 / 4.806 / 3.297 / 3.650 | 4.017 / 4.905 / 2.815 / 3.690 | +0.030 [-0.095, +0.202] | 5 / INCONCLUSIVE | 3.4-20.3 / 7.0-25.9 |
| mc2_24 | GameLogic | 2.056 / 1.296 / 0.901 / 0.937 | 2.116 / 1.299 / 0.915 / 0.977 | +0.034 [-0.015, +0.089] | 5 / INCONCLUSIVE | 3.4-20.3 / 7.0-25.9 |
| mc2_24 | DrawScreen | 1.846 / 2.243 / 1.632 / 1.715 | 1.887 / 2.306 / 1.633 / 1.762 | +0.040 [-0.029, +0.133] | 5 / INCONCLUSIVE | 3.4-20.3 / 7.0-25.9 |
| mc2_24 | SwapWindow | 0.293 / 0.338 / 0.266 / 0.276 | 0.006 / 0.006 / 0.003 / 0.005 | -0.278 [-0.291, -0.269] | 5 / WIN | 3.4-20.3 / 7.0-25.9 |
| battle | Frame | 12.496 / 30.370 / 4.841 / 6.907 | 11.690 / 30.501 / 4.851 / 7.140 | +0.552 [-1.357, +12.257] | 5 / INCONCLUSIVE | 13.6-59.7 / 2.8-56.2 |
| battle | GameLogic | 8.371 / 6.211 / 1.780 / 3.189 | 7.785 / 5.715 / 1.764 / 3.191 | +0.060 [-0.560, +3.122] | 5 / INCONCLUSIVE | 13.6-59.7 / 2.8-56.2 |
| battle | DrawScreen | 2.571 / 3.732 / 1.823 / 2.206 | 2.761 / 3.830 / 1.915 / 2.346 | +0.232 [-0.215, +2.298] | 5 / INCONCLUSIVE | 13.6-59.7 / 2.8-56.2 |
| battle | SwapWindow | 0.657 / 1.032 / 0.532 / 0.563 | 0.006 / 0.009 / 0.003 / 0.005 | -0.583 [-0.654, -0.553] | 5 / WIN | 13.6-59.7 / 2.8-56.2 |

## Host main/drawscreen

| Mission | Scope | A mean/frame / event p95 / min / Q1 ms | B mean/frame / event p95 / min / Q1 ms | Paired event-median delta [90% CI] ms | N / verdict | Reserved external CPU p95 A / B % |
|---|---|---:|---:|---:|---|---|
| mc2_01 | Frame | 1.336 / 1.582 / 0.830 / 1.243 | 1.364 / 1.600 / 0.857 / 1.262 | +0.003 [-0.003, +0.074] | 5 / INCONCLUSIVE | 3.8-11.6 / 5.2-15.0 |
| mc2_01 | GameLogic | 0.499 / 0.293 / 0.178 / 0.215 | 0.514 / 0.311 / 0.178 / 0.220 | +0.001 [+0.000, +0.012] | 5 / REGRESSION | 3.8-11.6 / 5.2-15.0 |
| mc2_01 | DrawScreen | 0.628 / 0.721 / 0.519 / 0.597 | 0.643 / 0.735 / 0.527 / 0.609 | +0.003 [-0.002, +0.028] | 5 / INCONCLUSIVE | 3.8-11.6 / 5.2-15.0 |
| mc2_01 | SwapWindow | 0.006 / 0.006 / 0.003 / 0.005 | 0.006 / 0.006 / 0.003 / 0.005 | +0.000 [-0.000, +0.000] | 5 / NO CHANGE | 3.8-11.6 / 5.2-15.0 |
| mc2_03 | Frame | 1.516 / 1.900 / 1.208 / 1.370 | 1.507 / 1.902 / 1.225 / 1.361 | +0.009 [-0.014, +0.042] | 5 / INCONCLUSIVE | 7.5-35.2 / 6.5-22.7 |
| mc2_03 | GameLogic | 0.611 / 0.364 / 0.253 / 0.284 | 0.604 / 0.355 / 0.253 / 0.283 | -0.004 [-0.006, +0.006] | 5 / NO CHANGE | 7.5-35.2 / 6.5-22.7 |
| mc2_03 | DrawScreen | 0.774 / 0.965 / 0.653 / 0.718 | 0.773 / 0.980 / 0.661 / 0.716 | +0.006 [-0.007, +0.046] | 5 / INCONCLUSIVE | 7.5-35.2 / 6.5-22.7 |
| mc2_03 | SwapWindow | 0.006 / 0.006 / 0.003 / 0.006 | 0.006 / 0.006 / 0.003 / 0.006 | -0.000 [-0.000, +0.000] | 5 / NO CHANGE | 7.5-35.2 / 6.5-22.7 |
| mc2_10 | Frame | 3.018 / 6.219 / 1.260 / 1.777 | 3.623 / 5.523 / 1.239 / 1.864 | +0.031 [-0.493, +0.803] | 5 / INCONCLUSIVE | 10.2-64.8 / 20.0-56.3 |
| mc2_10 | GameLogic | 1.494 / 1.943 / 0.336 / 0.402 | 1.303 / 1.514 / 0.330 / 0.420 | +0.001 [-0.195, +0.308] | 5 / INCONCLUSIVE | 10.2-64.8 / 20.0-56.3 |
| mc2_10 | DrawScreen | 1.333 / 2.939 / 0.707 / 0.842 | 1.259 / 2.495 / 0.706 / 0.861 | +0.021 [-0.181, +0.310] | 5 / INCONCLUSIVE | 10.2-64.8 / 20.0-56.3 |
| mc2_10 | SwapWindow | 0.008 / 0.009 / 0.003 / 0.006 | 0.007 / 0.008 / 0.003 / 0.006 | -0.000 [-0.000, +0.000] | 5 / NO CHANGE | 10.2-64.8 / 20.0-56.3 |
| mc2_24 | Frame | 6.712 / 9.848 / 3.603 / 5.575 | 6.135 / 9.290 / 3.513 / 5.012 | -0.501 [-1.844, +0.227] | 5 / INCONCLUSIVE | 40.9-50.4 / 22.1-61.7 |
| mc2_24 | GameLogic | 4.144 / 3.337 / 1.026 / 1.614 | 3.789 / 3.099 / 0.962 / 1.473 | -0.164 [-0.670, +0.071] | 5 / INCONCLUSIVE | 40.9-50.4 / 22.1-61.7 |
| mc2_24 | DrawScreen | 3.183 / 4.640 / 1.806 / 2.618 | 2.901 / 4.403 / 1.744 / 2.349 | -0.261 [-0.952, +0.111] | 5 / INCONCLUSIVE | 40.9-50.4 / 22.1-61.7 |
| mc2_24 | SwapWindow | 0.010 / 0.011 / 0.003 / 0.006 | 0.009 / 0.010 / 0.003 / 0.006 | -0.000 [-0.001, +0.000] | 5 / NO CHANGE | 40.9-50.4 / 22.1-61.7 |
| battle | Frame | 13.696 / 32.209 / 5.765 / 8.364 | 12.937 / 31.617 / 5.338 / 8.278 | -1.280 [-1.934, +78.259] | 5 / INCONCLUSIVE | 11.0-39.0 / 6.9-80.8 |
| battle | GameLogic | 9.334 / 6.629 / 2.043 / 3.752 | 8.831 / 6.106 / 2.023 / 3.703 | -0.429 [-0.605, +4.484] | 5 / INCONCLUSIVE | 11.0-39.0 / 6.9-80.8 |
| battle | DrawScreen | 3.193 / 4.483 / 2.098 / 2.697 | 3.052 / 4.341 / 2.078 / 2.641 | -0.267 [-0.381, +2.291] | 5 / INCONCLUSIVE | 11.0-39.0 / 6.9-80.8 |
| battle | SwapWindow | 0.007 / 0.011 / 0.004 / 0.006 | 0.007 / 0.011 / 0.004 / 0.006 | -0.000 [-0.001, +0.004] | 5 / NO CHANGE | 11.0-39.0 / 6.9-80.8 |

## Laptop W86/main

| Mission | Scope | A mean/frame / event p95 / min / Q1 ms | B mean/frame / event p95 / min / Q1 ms | Paired event-median delta [90% CI] ms | N / verdict | Reserved external CPU p95 A / B % |
|---|---|---:|---:|---:|---|---|
| mc2_01 | Frame | 5.834 / 11.703 / 1.865 / 3.777 | 5.936 / 12.313 / 2.306 / 3.640 | +0.063 [-0.139, +0.133] | 5 / INCONCLUSIVE | 15.6-59.1 / 16.0-68.6 |
| mc2_01 | GameLogic | 2.129 / 1.879 / 0.466 / 0.648 | 2.272 / 2.075 / 0.541 / 0.737 | +0.064 [+0.024, +0.082] | 5 / REGRESSION | 15.6-59.1 / 16.0-68.6 |
| mc2_01 | DrawScreen | 2.067 / 3.227 / 0.965 / 1.572 | 2.259 / 3.750 / 1.186 / 1.642 | +0.141 [+0.039, +0.679] | 5 / REGRESSION | 15.6-59.1 / 16.0-68.6 |
| mc2_01 | SwapWindow | 2.387 / 8.266 / 0.121 / 0.221 | 0.011 / 0.021 / 0.006 / 0.008 | -0.496 [-0.508, -0.376] | 5 / WIN | 15.6-59.1 / 16.0-68.6 |
| mc2_03 | Frame | 10.620 / 20.832 / 4.339 / 6.977 | 10.567 / 30.763 / 3.073 / 4.296 | -3.488 [-3.741, -3.296] | 5 / WIN | 15.7-20.6 / 16.6-18.8 |
| mc2_03 | GameLogic | 2.628 / 1.758 / 0.889 / 1.142 | 2.880 / 1.863 / 0.905 / 1.265 | +0.151 [+0.144, +0.164] | 5 / REGRESSION | 15.7-20.6 / 16.6-18.8 |
| mc2_03 | DrawScreen | 3.217 / 5.981 / 1.494 / 2.138 | 3.152 / 5.466 / 1.543 / 2.266 | +0.035 [-0.079, +0.086] | 5 / INCONCLUSIVE | 15.7-20.6 / 16.6-18.8 |
| mc2_03 | SwapWindow | 5.592 / 16.216 / 0.114 / 0.264 | 0.010 / 0.014 / 0.006 / 0.008 | -3.648 [-3.830, -3.501] | 5 / WIN | 15.7-20.6 / 16.6-18.8 |
| mc2_10 | Frame | 20.306 / 58.759 / 4.213 / 7.589 | 18.913 / 60.531 / 3.359 / 6.403 | -3.103 [-5.839, -2.832] | 5 / WIN | 14.9-21.6 / 14.1-27.4 |
| mc2_10 | GameLogic | 5.045 / 6.573 / 0.835 / 1.101 | 5.667 / 6.759 / 0.911 / 1.227 | +0.153 [-0.407, +0.257] | 5 / INCONCLUSIVE | 14.9-21.6 / 14.1-27.4 |
| mc2_10 | DrawScreen | 6.751 / 31.693 / 1.597 / 3.351 | 6.582 / 18.757 / 1.724 / 3.400 | +0.369 [-0.280, +0.501] | 5 / INCONCLUSIVE | 14.9-21.6 / 14.1-27.4 |
| mc2_10 | SwapWindow | 10.629 / 43.371 / 0.114 / 0.309 | 0.010 / 0.015 / 0.006 / 0.008 | -4.603 [-7.897, -4.123] | 5 / WIN | 14.9-21.6 / 14.1-27.4 |
| mc2_24 | Frame | 27.929 / 51.917 / 13.652 / 18.845 | 27.776 / 61.415 / 9.952 / 12.988 | -2.398 [-2.973, -1.154] | 5 / WIN | 14.7-28.5 / 17.4-24.8 |
| mc2_24 | GameLogic | 10.369 / 7.911 / 3.281 / 4.087 | 10.639 / 7.939 / 3.359 / 4.292 | +0.185 [+0.104, +0.384] | 5 / REGRESSION | 14.7-28.5 / 17.4-24.8 |
| mc2_24 | DrawScreen | 7.299 / 11.004 / 4.610 / 6.155 | 10.986 / 22.448 / 4.812 / 6.795 | +1.643 [+1.339, +1.706] | 5 / REGRESSION | 14.7-28.5 / 17.4-24.8 |
| mc2_24 | SwapWindow | 14.728 / 39.973 / 0.262 / 0.675 | 0.011 / 0.021 / 0.006 / 0.007 | -11.245 [-11.392, -10.444] | 5 / WIN | 14.7-28.5 / 17.4-24.8 |
| battle | Frame | 41.954 / 94.860 / 13.275 / 24.408 | 41.064 / 95.481 / 12.792 / 24.814 | +0.713 [-4.689, +4.132] | 5 / INCONCLUSIVE | 16.2-41.6 / 14.6-44.6 |
| battle | GameLogic | 18.063 / 12.024 / 4.684 / 7.597 | 17.952 / 11.447 / 5.062 / 7.787 | +0.153 [-0.026, +0.663] | 5 / INCONCLUSIVE | 16.2-41.6 / 14.6-44.6 |
| battle | DrawScreen | 6.573 / 9.970 / 3.900 / 5.340 | 6.896 / 9.854 / 3.757 / 5.505 | +0.298 [-0.145, +0.731] | 5 / INCONCLUSIVE | 16.2-41.6 / 14.6-44.6 |
| battle | SwapWindow | 2.131 / 7.112 / 0.615 / 0.819 | 0.010 / 0.026 / 0.007 / 0.008 | -0.925 [-1.031, -0.870] | 5 / WIN | 16.2-41.6 / 14.6-44.6 |

## Laptop main/drawscreen

| Mission | Scope | A mean/frame / event p95 / min / Q1 ms | B mean/frame / event p95 / min / Q1 ms | Paired event-median delta [90% CI] ms | N / verdict | Reserved external CPU p95 A / B % |
|---|---|---:|---:|---:|---|---|
| mc2_01 | Frame | 5.977 / 12.617 / 2.102 / 3.662 | 5.859 / 12.173 / 2.223 / 3.623 | -0.190 [-0.932, +0.149] | 5 / INCONCLUSIVE | 13.8-59.5 / 12.0-44.8 |
| mc2_01 | GameLogic | 2.273 / 2.027 / 0.498 / 0.733 | 2.242 / 2.008 / 0.527 / 0.720 | -0.010 [-0.155, +0.046] | 5 / INCONCLUSIVE | 13.8-59.5 / 12.0-44.8 |
| mc2_01 | DrawScreen | 2.245 / 3.727 / 1.122 / 1.677 | 2.195 / 3.656 / 1.147 / 1.668 | -0.052 [-0.886, +0.025] | 5 / INCONCLUSIVE | 13.8-59.5 / 12.0-44.8 |
| mc2_01 | SwapWindow | 0.011 / 0.020 / 0.006 / 0.008 | 0.011 / 0.020 / 0.006 / 0.008 | -0.000 [-0.002, +0.000] | 5 / NO CHANGE | 13.8-59.5 / 12.0-44.8 |
| mc2_03 | Frame | 10.519 / 30.791 / 3.203 / 4.307 | 10.522 / 30.576 / 3.020 / 4.278 | -0.055 [-0.072, +0.049] | 5 / INCONCLUSIVE | 15.6-23.8 / 15.7-25.5 |
| mc2_03 | GameLogic | 2.903 / 1.866 / 0.938 / 1.272 | 2.872 / 1.850 / 0.923 / 1.258 | -0.023 [-0.040, +0.027] | 5 / INCONCLUSIVE | 15.6-23.8 / 15.7-25.5 |
| mc2_03 | DrawScreen | 3.131 / 5.443 / 1.545 / 2.262 | 3.123 / 5.499 / 1.567 / 2.243 | -0.032 [-0.050, +0.016] | 5 / INCONCLUSIVE | 15.6-23.8 / 15.7-25.5 |
| mc2_03 | SwapWindow | 0.010 / 0.014 / 0.006 / 0.008 | 0.010 / 0.014 / 0.006 / 0.008 | -0.000 [-0.000, +0.000] | 5 / NO CHANGE | 15.6-23.8 / 15.7-25.5 |
| mc2_10 | Frame | 18.839 / 60.738 / 3.208 / 6.242 | 19.000 / 58.965 / 3.285 / 6.120 | -0.192 [-0.468, +0.214] | 5 / INCONCLUSIVE | 14.6-20.5 / 13.1-29.8 |
| mc2_10 | GameLogic | 5.575 / 6.768 / 0.919 / 1.214 | 5.558 / 6.754 / 0.953 / 1.224 | -0.078 [-0.124, -0.027] | 5 / WIN | 14.6-20.5 / 13.1-29.8 |
| mc2_10 | DrawScreen | 6.573 / 18.789 / 1.653 / 3.352 | 6.581 / 19.302 / 1.646 / 3.328 | +0.041 [-0.010, +0.231] | 5 / INCONCLUSIVE | 14.6-20.5 / 13.1-29.8 |
| mc2_10 | SwapWindow | 0.010 / 0.016 / 0.006 / 0.008 | 0.010 / 0.016 / 0.006 / 0.008 | +0.000 [-0.000, +0.000] | 5 / NO CHANGE | 14.6-20.5 / 13.1-29.8 |
| mc2_24 | Frame | 27.794 / 60.666 / 10.065 / 12.681 | 27.676 / 60.621 / 9.818 / 12.761 | -0.176 [-0.635, +1.151] | 5 / INCONCLUSIVE | 11.5-15.7 / 12.3-14.1 |
| mc2_24 | GameLogic | 10.487 / 7.820 / 3.462 / 4.240 | 10.486 / 7.847 / 3.361 / 4.262 | +0.007 [-0.043, +0.079] | 5 / INCONCLUSIVE | 11.5-15.7 / 12.3-14.1 |
| mc2_24 | DrawScreen | 11.137 / 23.153 / 4.809 / 6.691 | 11.046 / 22.571 / 4.761 / 6.710 | -0.012 [-0.127, +0.091] | 5 / INCONCLUSIVE | 11.5-15.7 / 12.3-14.1 |
| mc2_24 | SwapWindow | 0.009 / 0.021 / 0.006 / 0.007 | 0.009 / 0.021 / 0.006 / 0.007 | +0.000 [-0.000, +0.000] | 5 / NO CHANGE | 11.5-15.7 / 12.3-14.1 |
| battle | Frame | 37.591 / 76.686 / 12.536 / 24.320 | 35.579 / 77.056 / 12.812 / 23.672 | -1.237 [-3.142, +1.212] | 5 / INCONCLUSIVE | 22.5-34.4 / 21.5-29.0 |
| battle | GameLogic | 17.379 / 10.993 / 4.912 / 7.713 | 17.084 / 10.730 / 4.674 / 7.615 | -0.181 [-0.559, +0.153] | 5 / INCONCLUSIVE | 22.5-34.4 / 21.5-29.0 |
| battle | DrawScreen | 6.540 / 9.002 / 3.893 / 5.394 | 6.455 / 9.351 / 3.781 / 5.376 | -0.054 [-0.247, +0.281] | 5 / INCONCLUSIVE | 22.5-34.4 / 21.5-29.0 |
| battle | SwapWindow | 0.010 / 0.026 / 0.006 / 0.008 | 0.010 / 0.025 / 0.006 / 0.008 | -0.000 [-0.000, +0.001] | 5 / NO CHANGE | 22.5-34.4 / 21.5-29.0 |

## Ranked remaining target gaps

Drawscreen arm, measured fixed views; Frame budget 1 ms, DrawScreen budget 0.1 ms. Rows overlap and do not add. GPU budgets are not remeasured in this CPU assignment.

| Rank | Machine | Workload / scope | Mean ms | Budget ms | Gap ms |
|---:|---|---|---:|---:|---:|
| 1 | Laptop | battle / Frame | 35.579 | 1 | +34.579 |
| 2 | Laptop | mc2_24 / Frame | 27.676 | 1 | +26.676 |
| 3 | Laptop | mc2_10 / Frame | 19.000 | 1 | +18.000 |
| 4 | Host | battle / Frame | 12.937 | 1 | +11.937 |
| 5 | Laptop | mc2_24 / DrawScreen | 11.046 | 0.1 | +10.946 |
| 6 | Laptop | mc2_03 / Frame | 10.522 | 1 | +9.522 |
| 7 | Laptop | mc2_10 / DrawScreen | 6.581 | 0.1 | +6.481 |
| 8 | Laptop | battle / DrawScreen | 6.455 | 0.1 | +6.355 |
| 9 | Host | mc2_24 / Frame | 6.135 | 1 | +5.135 |
| 10 | Laptop | mc2_01 / Frame | 5.859 | 1 | +4.859 |
| 11 | Laptop | mc2_03 / DrawScreen | 3.123 | 0.1 | +3.023 |
| 12 | Host | battle / DrawScreen | 3.052 | 0.1 | +2.952 |
| 13 | Host | mc2_24 / DrawScreen | 2.901 | 0.1 | +2.801 |
| 14 | Host | mc2_10 / Frame | 3.623 | 1 | +2.623 |
| 15 | Laptop | mc2_01 / DrawScreen | 2.195 | 0.1 | +2.095 |
| 16 | Host | mc2_10 / DrawScreen | 1.259 | 0.1 | +1.159 |
| 17 | Host | mc2_03 / DrawScreen | 0.773 | 0.1 | +0.673 |
| 18 | Host | mc2_01 / DrawScreen | 0.643 | 0.1 | +0.543 |
| 19 | Host | mc2_03 / Frame | 1.507 | 1 | +0.507 |
| 20 | Host | mc2_01 / Frame | 1.364 | 1 | +0.364 |

## Three largest remaining DrawScreen subtrees

Largest distinct named camera passes, inner textureManagerRenderLists and Render.PostProcess in the heaviest successfully measured drawscreen workload. The activeScene and outer GameCamera::render textureManagerRenderLists wrappers are omitted to avoid ranking nested work twice. Inclusive elapsed ms per Frame, median across successful arm-B captures; these are costs, not promised savings.

| Machine | Workload | Subtree | ms/frame |
|---|---|---|---:|
| Host | battle | GameCamera::render particlesFlush | 0.791 |
| Host | battle | textureManagerRenderLists | 0.755 |
| Host | battle | GameCamera::render objects | 0.705 |
| Laptop | mc2_24 | textureManagerRenderLists | 6.928 |
| Laptop | mc2_24 | GameCamera::render objects | 3.087 |
| Laptop | mc2_24 | GameCamera::render shadows | 0.288 |

## Evidence and validation

The earlier MC2_MOVER_FIXTURE=200 path failed during appearance initialization on the simulation worker. Those failed captures remain in the original reports and supply no timings. Every battle number here is from a new five-pair main-thread-spawn sweep; the engine and its default simulation mode were unchanged.

Host reruns use the immutable `A:/Games/mc2-lane10-main-base` checkout at 586327c79. Refused live-main/stale-viewer deployments and captures at later main revisions were excluded. Both pinned payloads passed normal deployment checks, including the viewer and VC runtime files. The earlier valid W86/main stock sweep was retained only after auditing all 40 clean build receipts.

Raw traces, complete profile JSONs (all named zones), host-load samples, invocation logs, and private farm job receipts are under `E:/mc2-archive-2026-10/proofs/perf-robust/`. Farm fetch removes both per-job arm trees. Only `E:/mc2-installs/claude-perf-robust` was created on the host.

No engine cuts or default-OFF gates were added. Stats/job tests and the touched-tool contract rows passed; the five ratchets passed without baseline increases. The shared lane install keeps kit data fixed with the existing --skip-kit deploy option; engine payloads and manifest fingerprints still deploy normally.

Skipped steps: refused deployments from live main or stale viewer artifacts; these were replaced with normally validated pinned builds. Optional PowerShell profile autoload was skipped after an execution-policy denial. No provenance, lease or runtime check was disabled.
