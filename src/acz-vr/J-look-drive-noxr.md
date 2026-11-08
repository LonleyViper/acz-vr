# No-XR test sweep (addendum to J-look-drive-findings.md; offline only, NOT run live)

`xr_look_drive_test_sweep` also works WITHOUT `xr_stereo_output`: required keys are `gpu_copy_step5_continuous` + `xr_look_drive_keys` + `xr_look_drive_test_sweep`.
`Config::look_drive_latch(gate)` is the single rule: the real head-driven mode still needs `xr_stereo_output`; `xr_look_drive_keys` alone without XR latches
nothing; the sweep without the key latches nothing. In no-XR mode (`xr_look_drive_no_xr()`, log `mode=test-sweep-no-xr`): no pose provider is read (frame pose
invalid, IPD row-3 fallback as without XR), the head is the scripted sweep (`Driver::advance_sweep`), the producer still measures the rendered look each cockpit
frame and the controller presses keys from it; compensation, eye poses and consumer placement are skipped. All safety rules unchanged (foreground gate, kill file
`%LOCALAPPDATA%\acz-vr\look-drive.stop`, caps, watchdog, release). The per-second `look-drive;...` line has target, measured look, look-src, residual-rms.
Live: `tools/live/vr-test-start.ps1 -LookSweepNoXr [-EeCameraProbe]` writes log_enabled, diagnostic_ownership_pins, gpu_copy_witness_observation,
gpu_copy_early_device_hook, gpu_copy_step3_experiment, gpu_copy_step3_whole_set, gpu_copy_step5_continuous, gpu_copy_step5_members_by_class, xr_look_drive_keys,
xr_look_drive_test_sweep (+ ee_camera_probe). Unverified: producer + camera measurement with no consumer/pose provider live; key delivery to the foreground game.
