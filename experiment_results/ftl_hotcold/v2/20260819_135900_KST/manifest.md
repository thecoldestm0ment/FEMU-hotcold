# FEMU BBSSD FTL V2 correctness manifest

- Run ID: 20260819_135900_KST
- Branch: hotcold/v2
- Source commit: ffd7c8a70
- Parent implementation commit: 1efe61f24
- Corrected baseline result commit: 87814d23f
- Purpose: V2 implementation correctness only; 300-second performance experiment was not run

## V2 policy

- Default: T=1024, A=3, C=2, D=65536, decay ON
- Correctness override: D=8 to exercise lazy decay with a short sequence
- HOT condition: access_count >= A, latest interval <= T, short_interval_count >= C
- Saturating uint32 counters
- Per-LPN lazy epoch decay at host-write time
- GC reads stored state only; TRIM resets all LPN history

## Validation

- Build, git diff check, launcher shell syntax PASS
- COLD, COLD, HOT promotion sequence PASS
- lazy decay and HOT-to-COLD demotion PASS
- two-write reacquisition PASS
- DSM deallocate followed by first-write COLD reset PASS
- five GC moves preserved identical state/access/short/sequence/epoch PASS
- all printed NAND=Host+GC invariants PASS
- six invalid configuration cases returned exit code 1 before Guest boot

## Persistent limited sudo

The Guest image keeps /etc/sudoers.d/femu-experiments as root:root mode 0440. It passed visudo before and after installation. Existing exact baseline commands remain, and only these exact V2 correctness commands were added:

- 64 KiB sequential v2-seq fio at offset 4 KiB
- 30-second v2-gc fio with fixed target and arguments
- LPN0 DSM deallocate with fixed slbs and block count

No argument-free fio, nvme, blockdev, shell, editor, install, or arbitrary root command is passwordless. A normal password-required sudo group entry still exists and is not a passwordless rule.

Raw host/Guest logs are retained locally under the ignored raw_attempt directory. The temporary SSH public key was removed before clean Guest poweroff.
