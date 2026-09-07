# 누적 수정본 작업트리 인수인계 — 2026-09-07

## 무엇이 올라와 있는가

이 브랜치는 **누적 수정 전체를 담은 사람이 읽을 수 있는 unified diff 6개**를 보관합니다. 이 브랜치의 일반 `src/` 디렉터리가 이미 전체 통합본으로 바뀐 것은 아닙니다. 별도 `perf/verified-buffer-copy` 브랜치에는 버퍼 복사/패딩 최적화와 입력 경계 수정이 실제 소스 커밋 `70864d1a9e49cd99cea450585559c7da7f8d07fc`로 반영되어 있습니다.

아래 패치는 `5372b1bfc7a461704e08301edb6b393b20f3d2bb`를 기준으로 적용합니다. 적용 후 Git tree는 **`11234c36b0599c74ab2d79aaf807fb9d0f977060`**, tracked 파일은 **818개**입니다. 6개 원격 업로드 blob의 로컬 대응본을 `git apply --check --index` 및 `git apply --index`로 별도 작업트리에 적용하여 이 tree와 일치함을 확인했습니다.

같은 통합 소스는 별도로 제공된 Git bundle에서 커밋 `a72d48ae47acc3d736fe31466da5544cd3fa8dc1`로 복원할 수도 있습니다. 이 통합 커밋은 bundle에 있는 커밋이며, 이 handoff 브랜치의 HEAD와 다릅니다. 기존 PR #1/#2의 수정은 기준 커밋 이력에 이미 포함되어 있습니다.

**master, 사용자 PC, upstream은 변경하지 않았고 이번 작업에서 GitHub Actions는 실행하지 않았습니다.** 패치나 아래 명령을 게시한 것만으로 실행되는 자동화도 없습니다.

## GitHub에서 받은 패치로 F:에 별도 작업트리 만들기

기존 `gajae1/melonDS` clone 안에서 PowerShell로 실행합니다. 아래 두 F: 경로와 `local/verified-modernization` 브랜치는 아직 없어야 합니다. 기존 폴더를 덮어쓰지 마십시오.

```powershell
git fetch origin handoff/verified-modernization perf/verified-buffer-copy
if ($LASTEXITCODE -ne 0) { throw 'fetch failed' }
git worktree add --detach F:/melonDS-patches origin/handoff/verified-modernization
if ($LASTEXITCODE -ne 0) { throw 'patch worktree creation failed' }
git worktree add -b local/verified-modernization F:/melonDS-verified 5372b1bfc7a461704e08301edb6b393b20f3d2bb
if ($LASTEXITCODE -ne 0) { throw 'source worktree creation failed' }
$patches = @(Get-ChildItem F:/melonDS-patches/handoff/*-integration.patch | Sort-Object Name | ForEach-Object { $_.FullName })
if ($patches.Count -ne 6) { throw 'Expected exactly six patches' }
git -C F:/melonDS-verified apply --check --index @patches
if ($LASTEXITCODE -ne 0) { throw 'Patch validation failed; source not modified' }
git -C F:/melonDS-verified apply --index @patches
if ($LASTEXITCODE -ne 0) { throw 'Patch application failed; inspect the isolated worktree' }
$tree = git -C F:/melonDS-verified write-tree
if ($LASTEXITCODE -ne 0 -or $tree.Trim() -ne '11234c36b0599c74ab2d79aaf807fb9d0f977060') { throw 'Integrated source tree mismatch' }
git -C F:/melonDS-verified status --short
```

결과는 새 작업트리의 **staged 변경**입니다. 자동 commit/push/master merge는 하지 않습니다. macOS/Linux에서는 동일한 base와 패치 6개의 절대경로를 사용하면 됩니다. 패치 내용은 독립 커밋이 아니라 큰 unified diff를 파일 경계에서 나눈 것이므로 전부 순서대로 적용해야 합니다.

## 실행한 검증

이번 추가 작업의 최종 결과이며, 통합 소스 안의 이전 배치 문서에 나오는 21/19/17개 숫자를 대체합니다.

| 구성 | 확인 결과 |
| --- | --- |
| Clang 17 Release, 전체 headless 코어/Teakra/JIT | 28/28 CTest 통과 |
| GCC 14.2 Release, 전체 코어, ASM/JIT/SIMD OFF | 26/26 통과 |
| Clang 17 ASan/UBSan, standalone 경계 테스트 | 23/23 통과 |
| OpenGL/compute 소스 포함 전체 core 정적 라이브러리 | 컴파일 통과; GPU 실행 아님 |
| 원격 소스 브랜치의 버퍼 전용 sanitizer 테스트 | 4/4 통과 |

실행 환경은 **AVX2 지원, AVX-512F 미지원**입니다. AVX-512는 컴파일 및 미지원 시 기본 구현 선택을 검사했으며 native AVX-512 실행 결과가 아닙니다. ARM interpreter/JIT/fast-memory JIT에서 직접 작성한 명령과 CLZ 결과, 프레임 실행, 저장/복원, 소프트웨어 화면 픽셀을 검사했습니다. 실제 DS/DSi 기기나 상용 게임을 비교한 검증은 아닙니다.

신규 수정: ROM 버퍼 overflow 무한 반복/null 충돌 및 불필요한 초기화 제거; std::bit_ceil/bit_floor/rotr/countl_zero 사용; 기존 ARM7 CLZ 예외 경로/플래그/사이클 호출 유지; GBA 원본 길이 검사; NDS 헤더 범위 덧셈 overflow 방지. 이전 C23/C++26, Teakra, 네트워크, SIMD 및 캡처 수정도 포함합니다.

동일 GCC 14.2 옵션으로 준비 실행 후 순서를 섞은 7쌍씩의 독립 측정에서 CLZ 3.225054→1.510439 ns/op, 3 MiB+17 바이트 복사 139323.15→91265.22 ns, 패딩 195644.27→126812.47 ns였습니다. 짝별 checksum은 일치했습니다. **전체 에뮬레이터 FPS 향상률이 아닙니다.**

## 버전과 남은 검증

CMake 설치 pin은 4.4.3으로 갱신했지만 여기서 실제 빌드에 사용한 CMake는 3.31.6입니다. 최신 도구체인 profile의 Clang 23.1.0/GCC 16.2, vcpkg 변경 및 외부 Qt profile은 설치/전체 프런트엔드 통합을 완료한 결과가 아닙니다. `tools/ci-current-build.py`와 libarchive overlay 생성 helper는 이 작업에서 실행하지 않았습니다. 해당 helper가 존재한다고 Actions가 실행되거나 의존성이 설치되는 것은 아닙니다.

Windows/macOS/BSD 전체 프런트엔드, 실제 GPU 출력/readback 성능, 모든 DSP 명령/타이밍, 실기·상용 게임 회귀와 최신 의존성 집합의 전체 빌드는 미검증입니다. 전체 코드 최적화 완료나 무회귀를 보증하지 않습니다. 새 언어, 전역 fast-math, 강제 AVX-512, 새 수동 ASM은 추가하지 않았습니다.
