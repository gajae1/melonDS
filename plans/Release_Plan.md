# 버전별 5페이즈 계획

1.1.02를 기준으로 DS 기능을 유지하며 재현된 결함을 수정하고 실험 기능의 구현·복구·호환성을 완성한다.
42개 책임 버전(1.1.03~1.1.44), 210개 페이즈다. 달력 일정이나 구현 완료 선언은 아니다.
버전의 patch는 숫자로 증가한다. 표시 버전, 저장 상태 형식, 통신 프로토콜 버전을 구분한다.
작은 결함 수정은 원 ID를 유지해 앞 버전에 선반영할 수 있다. 실제 배포 범위와 남은 과제는 릴리스별 기록을 따른다.

각 버전은 ① 재현·설계 ② 최소 구현 ③ 통합·복구 ④ 검증 ⑤ 출시 판정 순서다.
정확성·데이터 보존 수정은 반례 해결과 관련 회귀로 수락한다. 성능 비용은 별도로 공개한다.
성능만을 위한 변경은 동일 출력과 실제 workload의 이득이 모두 있어야 채택한다.
언어 모드·최신 라이브러리·ISA 지원만으로 속도나 호환성 향상을 추정하지 않는다.

## 선행 산출물과 수락

- 선행 버전은 통합 계약이다. 독립 재현과 필요한 부분 구현을 전체 버전 완료까지 지연하지 않는다.
- BV-04의 기존 로컬 명령과 BV-03/05의 산출물 선별은 첫 검증·배포부터 사용한다. 배포 도구 완성은 1.1.43이다.
- 1.1.27/28은 해당 커널의 GR-14 계측만 먼저 요구한다. 변경하는 3D 경계에만 GR-08 기대값을 요구해 역방향 대기를 피한다.
- 1.1.18 부팅 대조는 AD-09~16 전체 구현 완료를 요구하지 않는다. 필요한 I/O 판정만 별도로 전달한다.
- AD-26의 기존 DSP 명령군·표본 대조는 AD-18의 새 실기 결과 대기와 독립적으로 진행한다.
- NAND 오류 수정은 합성 이미지·분리한 시험 사본으로 시작한다. 새 overlay는 별도 기능 채택 사항이다.
- PGO 수집·병합·사용 도구는 계획 대상이다. 기존 LTO 설정이 PGO 구현 완료를 뜻하지 않는다.
- 최종 수락은 배포에 포함된 SPU/BMI/Software 2D·3D/SDL3(1.1.09/25/27/28/37)의 근거도 확인한다. 미포함 옵션은 필수 선행으로 만들지 않는다.
- ARM64·Apple·BSD 지원은 각 native 실행 근거로 수락한다. 다른 플랫폼의 성공 결과로 대체하지 않는다.

CTest 정규식은 기존 회귀의 시작점이다. 새 후보 검증을 자동 보장하지 않으며 빈 선택을 실패로 처리한다.
[검증 방법](Validation.md)과 [착수 순서](Execution_Order.md)를 함께 읽는다. 모든 페이즈는 아래 배포 기록에서 증거를 연결하기 전까지 계획 상태다.

## 버전 색인

| 버전 | 범위 | 통합 선행 |
|---|---|---|
| [1.1.03](#v03) | 재현 기준과 검증 자료 정리 | 1.1.02 기준 |
| [1.1.04](#v04) | 게임 저장·종료·충돌 복구 | 1.1.03 |
| [1.1.05](#v05) | 저장 상태의 로드·복원 원자성 | 1.1.04 |
| [1.1.06](#v06) | 입력·포커스·컨트롤러·터치 | 1.1.03 |
| [1.1.07](#v07) | 프런트엔드 파일 입력·설정·작업 취소 | 1.1.04, 1.1.05 |
| [1.1.08](#v08) | 호스트 오디오·마이크·장치 재연결 | 1.1.03, 1.1.06 |
| [1.1.09](#v09) | SPU 보간·ADPCM·capture 정확성 | 1.1.08 |
| [1.1.10](#v10) | GDB 프로토콜·CPU 레지스터·연결 수명 | 1.1.03 |
| [1.1.11](#v11) | PCap·Slirp 네트워크 백엔드 | 1.1.03 |
| [1.1.12](#v12) | Wi-Fi·LocalMP·LAN 세션 안정성 | 1.1.03 |
| [1.1.13](#v13) | RTC·치트 입력·편집 데이터 보존 | 1.1.04, 1.1.05 |
| [1.1.14](#v14) | x64 JIT 명령·플래그·예외 정확성 | 1.1.03 |
| [1.1.15](#v15) | 메모리·CP15·DMA·IRQ·타이머 | 1.1.14 |
| [1.1.16](#v16) | OpenGL context·캡처·자원 수명 | 1.1.03 |
| [1.1.17](#v17) | Compute renderer 정확성·동기화 | 1.1.16 |
| [1.1.18](#v18) | DSi 부팅·firmware·원본 대조 | 1.1.03 |
| [1.1.19](#v19) | DSi NAND·SD·파일 시스템 복구 | 1.1.04, 1.1.05, 1.1.18 |
| [1.1.20](#v20) | DSi SCFG·NWRAM·NDMA·reset 타이밍 | 1.1.15, 1.1.18 |
| [1.1.21](#v21) | DSP modulo·메모리·플래그 결함 | 1.1.03 |
| [1.1.22](#v22) | DSP 전체 명령·이벤트·타이밍 검증 | 1.1.20, 1.1.21 |
| [1.1.23](#v23) | DSi 카메라·I2C·전원 주변장치 | 1.1.18, 1.1.20 |
| [1.1.24](#v24) | 카트리지·homebrew·GBA 슬롯 주변장치 | 1.1.04, 1.1.05, 1.1.06, 1.1.15 |
| [1.1.25](#v25) | BMI·block cache·JIT 성능 | 1.1.14, 1.1.15 |
| [1.1.26](#v26) | ISA 분기·CRC·hash·공통 커널 | 1.1.03 |
| [1.1.27](#v27) | 소프트웨어 2D 합성·밝기 SIMD | 1.1.15, 1.1.26 |
| [1.1.28](#v28) | 소프트웨어 3D fog·AA·texture 처리 | 1.1.27 |
| [1.1.29](#v29) | DSi AES·CTR/CCM·하드웨어 crypto | 1.1.19, 1.1.26 |
| [1.1.30](#v30) | 표시 지연·프레임 pacing·전송 비용 | 1.1.08, 1.1.16, 1.1.17 |
| [1.1.31](#v31) | Netplay의 실제 게임 경로 완성 | 1.1.05, 1.1.12 |
| [1.1.32](#v32) | Vulkan 장치·창·기본 표시 | 1.1.16, 1.1.30 |
| [1.1.33](#v33) | Vulkan 3D rasterization | 1.1.32 |
| [1.1.34](#v34) | Vulkan 2D·display capture·CPU readback | 1.1.16, 1.1.33 |
| [1.1.35](#v35) | Vulkan cache·장치 손실·안정 승격 | 1.1.30, 1.1.34 |
| [1.1.36](#v36) | 효용 중심 C23·C++26·의존성 갱신 | 1.1.03 |
| [1.1.37](#v37) | SDL3 전환과 장치 API 통합 | 1.1.06, 1.1.08, 1.1.36 |
| [1.1.38](#v38) | LTO·PGO·컴파일러 최적화 | 1.1.03, 1.1.14, 1.1.36 |
| [1.1.39](#v39) | ARM64 JIT 정확성과 native 실행 | 1.1.14 |
| [1.1.40](#v40) | ARM64 native·NEON·기본 확장 검증 | 1.1.26, 1.1.39 |
| [1.1.41](#v41) | 고급 ARM 확장·Apple 플랫폼 조건부 지원 | 1.1.40 |
| [1.1.42](#v42) | Linux·BSD 프런트엔드 수락 | 1.1.06, 1.1.08, 1.1.12, 1.1.16, 1.1.36 |
| [1.1.43](#v43) | 버전·패키지·복구 가능한 배포 | 1.1.04, 1.1.07, 1.1.36 |
| [1.1.44](#v44) | 전체 게임·실기·실험 기능 승격 검증 | 1.1.05, 1.1.10, 1.1.11, 1.1.12, 1.1.13, 1.1.19, 1.1.20, 1.1.22, 1.1.23, 1.1.24, 1.1.29, 1.1.30, 1.1.31, 1.1.35, 1.1.38, 1.1.43 |

<a id="v03"></a>

## 1.1.03 — 재현 기준과 검증 자료 정리

기본 과제: CJ-20, BV-01, BV-10, BV-11, BV-18.

소스 시작점: [ROMSmoke.cpp](../tests/ROMSmoke.cpp), [CoreExecution.cpp](../tests/CoreExecution.cpp), [Sanitizers.cmake](../cmake/Sanitizers.cmake).

기존 회귀 시작점: `core-.*execution|firmware-profile`.

1. 1.1.02 소스·빌드 옵션·합성 입력·게임 smoke의 기준을 고정하고, 기존 완료/미완료 대조표를 확정한다.
2. ROMSmoke의 기존 frame/key script를 확장할 최소 event 형식을 정한다. 터치·RTC·lid·mic 중 실제 재현에 필요한 입력부터 추가한다. 검증 자료의 자동 수집 대신 실행자가 선택한 재현 입력을 사용한다.
3. 검증 결과에 source revision·CPU/GPU/driver·renderer·JIT·입력 ID·실패/skip·화면/상태 digest를 연결한다. 공개 결과에는 재현에 필요한 비식별 정보만 남긴다. 기존 CMake 명령을 재사용해 로컬 baseline·sanitizer 사용표를 만들며 JIT의 의도된 fault와 실제 오류를 구분한다.
4. 기존 CoreExecution·FirmwareProfile·ROMSmoke에서 재현성이 유지되는지 확인하고, 오류 화면/중단/빈 corpus를 성공으로 세지 않는지 검사한다.
5. 재현 명령·실패 표본·검증 범위를 묶는다. 이후 버전은 이 기준과 비교하고 이미 통과한 동일 조건을 재사용한다.

첫 증분 구현은 [1.1.03 기록](releases/1.1.03.md)에 연결한다. 이 증분만으로 CJ-20/BV-10/11/18 전체를 완료 처리하지 않는다.

<a id="v04"></a>

## 1.1.04 — 게임 저장·종료·충돌 복구

기본 과제: FS-03, FS-05, FS-06, FS-13.

소스 시작점: [SaveManager.cpp](../src/frontend/qt_sdl/SaveManager.cpp), [EmuInstance.cpp](../src/frontend/qt_sdl/EmuInstance.cpp), [SaveManagerIO.cpp](../tests/SaveManagerIO.cpp).

기존 회귀 시작점: `save-manager-`.

1. ROM 교체 실패 뒤 이전 save manager/경로가 남는 조합, 같은 basename 게임의 저장 충돌, worker flush 중 경로·버퍼 변경, 종료까지 실패가 지속되는 상황을 분리해 재현한다.
2. 기존 mutex로 path·buffer·version의 소유권을 일관되게 하고 새 카트·경로·save manager는 성공 시 함께 교체한다. 마지막 성공/미저장 상태와 충돌을 호출자에 전달하며 기존 파일명 자동 이관은 하지 않는다.
3. 종료/게임 변경/instance 종료에 bounded retry와 사용자 복구 수단을 연결한다. 기존 파일 덮어쓰기 fallback은 켜지 않는다.
4. 잠금 유지/해제·없는 폴더·동시 요청·종료 race·합성 short write를 확인하고, 실제 저장장치 부족·분리 검사는 별도 결과로 기록한다.
5. 이전 save 원본·복구 가능한 최신 buffer·정상 재시작을 확인한 뒤 배포한다. 지속 실패를 저장 성공으로 표시하면 출시하지 않는다.

FS-05 잠금은 [1.1.04](releases/1.1.04.md), FS-03 교체 준비와 FS-06 종료 선택은 [1.1.05](releases/1.1.05.md), FS-13 이름 선택·경로 고정은 [1.1.10](releases/1.1.10.md)에 반영한다. 실제 게임·장치 수락과 완전한 저장 이관은 후속 범위다.

<a id="v05"></a>

## 1.1.05 — 저장 상태의 로드·복원 원자성

기본 과제: CJ-09, CJ-10, FS-01. 공동·후속: FS-02, AD-05, NP-13.

소스 시작점: [Savestate.cpp](../src/Savestate.cpp), [EmuInstance.cpp](../src/frontend/qt_sdl/EmuInstance.cpp), [SavestateFileIO.cpp](../tests/SavestateFileIO.cpp).

기존 회귀 시작점: `savestate-|core-.*execution`.

1. 현재 loadState/undo 경로의 상태 변화 순서와 실패 지점을 고정한다. 게임/console mode/형식 불일치의 기대 동작을 정의한다. 활성 event FuncID=3/미등록 callback과 뒤쪽 section 실패, undo 실패 반환을 최소 입력에 포함한다.
2. 길이·section·객체 상태를 적용 전에 검증하고, 적용 중 실패해도 이전 실행 상태로 복귀할 경계를 설계·구현한다. rollback 자체가 실패하면 부분 상태로 실행하지 않고 명시적 정지/복구 상태로 전환한다.
3. cart/renderer/DSP·오디오 history·진행 중 저장 작업과 state 교체의 수명을 연결한다. 코어 상태 복원, host queue 재개, 외부 NAND/SD·네트워크의 이미 발생한 부작용을 별도 상태 영역으로 기록한다. 코어 rollback 성공을 외부 매체·상대 peer까지 되돌렸다는 의미로 사용하지 않는다. 필요한 format migration은 명시적으로 version한다.
4. 잘린/과대/잘못된 section·다른 ROM·오래된 state·로드 중 오류를 넣고 이전 레지스터·메모리·save를 보존하는지 확인한다.
5. 성공/실패 후 게임 진행·다시 저장·재시작을 확인한다. 부분 로드된 세션이 남으면 안정 버전으로 내보내지 않는다.

CJ-09 이벤트 입력은 1.1.03, CJ-10/FS-01의 복원·결과 전파와 FS-02 상태 파일 읽기는 [1.1.04](releases/1.1.04.md)에 선반영한다. 다른 카트·형식·DSi 장치와 host queue·외부 부작용의 전체 수락은 계속 필요하다.

<a id="v06"></a>

## 1.1.06 — 입력·포커스·컨트롤러·터치

기본 과제: FS-09, FS-10, FS-11, FS-12. 공동·후속: NP-17.

소스 시작점: [EmuInstanceInput.cpp](../src/frontend/qt_sdl/EmuInstanceInput.cpp), [KeyboardInput.cpp](../src/frontend/qt_sdl/KeyboardInput.cpp), [InputConfigDialog.cpp](../src/frontend/qt_sdl/InputConfig/InputConfigDialog.cpp), [Window.cpp](../src/frontend/qt_sdl/Window.cpp).

기존 회귀 시작점: `qt-keyboard-mapping-input`.

1. 기존 정상 키 매핑을 보존하며 device ID·focus·modifier·auto-repeat·터치 좌표 변환의 소유자를 정한다. GameController→일반 joystick 전환 뒤 stale handle, touch lastPosition 사용·cancel·focus 상실을 구체 반례로 고정한다.
2. 분리/재연결·축 경계·여러 창·키/터치 release 누락의 재현 사례를 먼저 수정한다. 변경한 버튼 상태의 소비 시점도 확인한다. 닫은 핸들과 capability를 함께 비우고 touch 좌표·눌림을 같은 snapshot으로 전달한다.
3. 매핑 대화상자의 Apply/Cancel과 instance별 controller 선택을 연결한다. DPI·rotation·화면 밖 drag release를 같은 좌표 규칙에 맞춘다.
4. Qt 실제 이벤트와 패드 hotplug/터치 모서리를 확인한다. 입력 timestamp로 poll 지연을 구분하고 실제 입력→화면은 장치 시험으로 남긴다.
5. 사용자 기존 설정 재열기·게임 입력·창 전환·종료 후 stuck input이 없는 범위를 배포한다. 새로운 modifier 동작은 선택형으로 제공한다.

FS-09/10의 장치 수명과 터치 이벤트·좌표 게시 경계는 [1.1.07 증분](releases/1.1.07.md)에서 다룬다. FS-11/12의 전체 창·DPI·장치 identity 수락은 별도다.

[1.1.14 증분](releases/1.1.14.md)은 장치 연결 시 키보드 설정이 숨겨지는 UI 경로를 수정하고, 실제 매핑 창→저장→실행 적용→프로세스 재시작을 검증한다. 이는 사용자 지정 키 전체가 고정값으로 대체되는 문제나 JIT 책임 과제 완료를 의미하지 않는다.

<a id="v07"></a>

## 1.1.07 — 프런트엔드 파일 입력·설정·작업 취소

기본 과제: FS-02, FS-04, FS-07, FS-08, FS-20.

소스 시작점: [ArchiveUtil.cpp](../src/frontend/qt_sdl/ArchiveUtil.cpp), [Config.cpp](../src/frontend/qt_sdl/Config.cpp), [EmuInstance.cpp](../src/frontend/qt_sdl/EmuInstance.cpp), [ROMInfoDialog.cpp](../src/frontend/qt_sdl/ROMInfoDialog.cpp).

기존 회귀 시작점: `nds-rom-header-bounds|gba-rom-input-bounds|utf8-path|firmware-profile`.

1. archive→선택→ROM 교체·취소와 설정 Apply/Cancel의 상태 전이를 고정한다. 실제 지원 형식과 메모리 상한을 목록화한다. Zstd의 알려지지 않은 content size, 손상 TOML 시작→종료, 저장 가져오기 검증 전 reset도 포함한다.
2. 불완전 archive·음수/과대 길이·중복 항목·UTF-8 경로·파일 변경에 필요한 bounded read와 오류 반환을 구현한다. Zstd는 실제 출력 길이와 frame 완료·할당 실패를 검사하고 TOML은 임시 parse 성공 후 교체해 손상 원본을 보존한다.
3. 실패하거나 취소된 열기가 현재 게임·최근 목록·기존 설정을 파괴하지 않게 연결한다. Profile Korean과 BIOS role 안내는 기존 동작 위에서 개선한다.
4. ZIP/7z의 작은 합성 손상 입력, 긴/한글 경로, 폴더 이동, ROM 재선택, 대화상자 취소를 실제 frontend 경계에서 확인한다.
5. 지원 형식·실패 메시지·취소 후 정상 게임·portable 설정을 확인해 배포한다. UI 전면 번역·게임 라이브러리 확장은 별도 Future 후보로 유지한다.

FS-02의 ROM/import와 FS-07의 archive 경계는 [1.1.06 증분](releases/1.1.06.md)에 반영한다. 비동기 준비·취소와 전체 파일·형식 수락은 후속 범위다.

<a id="v08"></a>

## 1.1.08 — 호스트 오디오·마이크·장치 재연결

기본 과제: AD-01, AD-02, AD-04, AD-05. 공동·후속: AD-21.

소스 시작점: [EmuInstanceAudio.cpp](../src/frontend/qt_sdl/EmuInstanceAudio.cpp), [AudioLowPass.h](../src/frontend/qt_sdl/AudioLowPass.h), [AudioSettingsDialog.cpp](../src/frontend/qt_sdl/AudioSettingsDialog.cpp), [FrontendAudio.cpp](../tests/FrontendAudio.cpp).

기존 회귀 시작점: `audio-callback-buffer|audio-core-clock`.

1. core sample 생성, host queue, callback, 장치 reopen의 clock/lock 소유권을 정리하고 underrun/crackle 재현 조건을 고정한다. micResample의 마지막 이웃 sample 접근과 lock 밖 micExtBufferCount 읽기를 분리한 재현 입력을 준비한다.
2. rate 변경·mute/pause·장치 분리·record/playback 실패·buffer 상한의 결함을 수정한다. callback에서 새 할당/긴 lock/반복 로그를 피한다. mic block 끝 보간과 count 검사/소비를 같은 수명 계약으로 보호한다.
3. 설정 변경의 재시작·취소·filter history·마이크 선택을 연결하고, queue/latency 진단은 opt-in의 작은 카운터로 구현한다.
4. 합성 stereo/mic 신호와 장치 rate·분리/복구·장기 queue를 비교한다. 유선/USB/Bluetooth의 실제 지연·청취는 별도로 측정한다.
5. 기본 출력과 filter OFF의 호환성을 확인해 배포한다. 시간 늘이기/clock drift 보정은 증거가 있는 별도 opt-in으로만 승격한다.

AD-01/02의 마이크 입력 끝과 queue/FPS 공유 경계는 [1.1.07 증분](releases/1.1.07.md)에서 다룬다. 출력 장치·크랙·실측 지연·전체 재연결 수락은 계속 필요하다.

<a id="v09"></a>

## 1.1.09 — SPU 보간·ADPCM·capture 정확성

기본 과제: AD-03, AD-06, AD-07, AD-08, AD-25.

소스 시작점: [SPU.cpp](../src/SPU.cpp), [SPU.h](../src/SPU.h), [AudioResampling.cpp](../tests/AudioResampling.cpp), [TeakraExecution.cpp](../tests/TeakraExecution.cpp).

기존 회귀 시작점: `audio-core-clock|teakra-execution`.

1. 채널 timer·ADPCM loop/history·Gaussian/cubic·pan·capture 순서와 현재 rounding을 식으로 고정한다. 최초 SPU 생성·재사용의 DS/DSi Auto/10/16-bit 정책과 one-shot hold·capture 소스 선택을 확인한다.
2. 음수 경계·loop start·capture wrap/IRQ·volume/bias 상태에서 확인한 결함을 최소 수정한다. 항별 shift와 합산 후 shift를 합치지 않는다. bitdepth 초기값 덮어쓰기 후보와 hold/capture 모드는 독립 oracle로 확인한 경우 수정한다.
3. 정확성이 확보되고 실제 비용이 확인된 연속 샘플/독립 채널에 한해 SIMD kernel의 필요성을 판단한다. 후보가 없으면 기존 경로를 유지하며 guest capture는 frontend 필터와 분리한다.
4. 합성 signed 경계·sample clock·capture 메모리·reset/state roundtrip을 scalar와 비교하고 실제 소리/실기 sample 검증을 구분한다.
5. SPU 초기화·hold·capture 등의 정확성 수정은 해당 oracle·회귀·장치 gate로 판정한다. 선택적 성능 kernel은 정확성 일치와 목표 workload 이득을 모두 얻었을 때만 활성화한다. 보간 방식의 음질 선호는 정확성 모드와 별도 사용자 선택으로 제공한다.

<a id="v10"></a>

## 1.1.10 — GDB 프로토콜·CPU 레지스터·연결 수명

기본 과제: CJ-11, FS-18, NP-10, NP-11, NP-12.

소스 시작점: [GdbProto.cpp](../src/debug/GdbProto.cpp), [GdbCmds.cpp](../src/debug/GdbCmds.cpp), [GdbStub.cpp](../src/debug/GdbStub.cpp), [ARM.cpp](../src/ARM.cpp), [EmuSettingsDialog.cpp](../src/frontend/qt_sdl/EmuSettingsDialog.cpp).

기존 회귀 시작점: `core-interpreter-execution`.

1. 최대 packet+framing, M/X의 짧은 body·escape·주소 wrap, no-ack 후 재접속, 활성 SVC의 SPSR read/write를 로컬 입력으로 재현한다.
2. payload/encoded frame/terminator의 공통 용량과 광고 PacketSize를 맞추고 메모리 쓰기는 완전한 명령 검증 뒤 수행한다. binary X를 hex와 분리한다.
3. SPSR bank와 CPSR 상태 계약을 보정하고 연결의 EOF·부분 송신·재접속 초기화·취소 가능한 종료를 정리한다. 기존 JIT 동시 사용 제한은 유지한다.
4. guard와 무부분쓰기 검사 후 실제 GDB로 ARM7/ARM9 연결·메모리 load·exception return·reset·pause 중 detach/닫기를 검증한다. Windows/Linux socket 차이와 포트 충돌을 확인한다.
5. 반복 디버깅 뒤 게임이 정상 진행하고 새 클라이언트가 깨끗한 상태에서 연결되는 결과를 남긴다. 지원 명령·최대 packet·빌드 옵션을 제품 설정과 일치시킨다.

<a id="v11"></a>

## 1.1.11 — PCap·Slirp 네트워크 백엔드

기본 과제: NP-05, NP-06, NP-07.

소스 시작점: [Net_PCap.cpp](../src/net/Net_PCap.cpp), [Net_Slirp.cpp](../src/net/Net_Slirp.cpp), [LAN.cpp](../src/net/LAN.cpp).

기존 회귀 시작점: `network`.

1. 심볼이 없는 PCap library, caplen<len, 비Ethernet link, 짧은 IPv4/UDP/DNS와 잘린 label을 재현 표본으로 고정한다.
2. PCap 라이브러리·adapter 자원 소유를 하나로 정리하고 불완전 캡처와 오류를 수신 경계에서 거부한다. 단순히 길이를 줄여 손상 frame을 전달하지 않는다.
3. Slirp 우회 parser의 실제 남은 길이·IHL·label cursor를 검증하고 지원하지 않는 입력을 명시적으로 처리한다. DNS 비동기화는 지연이 관측된 경우에만 최소 단위로 시행한다.
4. 정상 DNS 응답·오류 응답·최소 경계 입력과 loader 실패 후 재시도를 검사한다. 실제 Windows Npcap 설치/미설치와 Linux libpcap, 유선/무선 장치 중단을 구분한다.
5. 기본 네트워크 동작·오류 메시지·종료 복구가 동일하게 유지되는 패키지를 만든다. 외부 서비스 성공 여부와 로컬 transport 결과를 분리해 지원 범위를 적는다.

<a id="v12"></a>

## 1.1.12 — Wi-Fi·LocalMP·LAN 세션 안정성

기본 과제: FS-17, NP-01, NP-02, NP-03, NP-04, NP-08. 공동·후속: CJ-08, CJ-20.

소스 시작점: [Wifi.cpp](../src/Wifi.cpp), [DSi_NWifi.cpp](../src/DSi_NWifi.cpp), [LocalMP.cpp](../src/net/LocalMP.cpp), [LAN.cpp](../src/net/LAN.cpp), [LANDialog.cpp](../src/frontend/qt_sdl/LANDialog.cpp).

기존 회귀 시작점: `network|core-.*execution`.

1. 실제 길이보다 큰 LAN payload·잘못된 AID/sender·빈 handshake·수신 중지 후 FIFO wrap을 공개 합성 입력으로 재현한다. 정상 패킷의 특수 값과 길이를 먼저 고정한다.
2. LAN 수신 경계에서 길이·종류·peer 관계를 검증하고 packet 소유권·player 목록 동기화를 정리한다. UI 연결 취소와 종료가 같은 세션을 한 번만 정리하게 한다.
3. LocalMP의 넘침·부분 기록·호스트 소실 복구 정책을 구현한다. DS retry 및 DSi scan/connect 실패 event는 실제 trace로 입증한 전이만 보정한다.
4. 정상 cmd/reply/ack와 잘못된 입력, 느린 인스턴스·pause/resume·재접속을 검사한다. 두 PC 실제 게임 참가·통신·호스트 종료, DSi 장치 시퀀스는 별도 gate로 기록한다.
5. 지원하는 한 그룹/복수 그룹 범위와 실패 상태를 명시한다. 무한 대기·다른 세션 오염 없이 로컬 플레이로 돌아오고 저장이 지속되어야 안정 기능으로 배포한다.

<a id="v13"></a>

## 1.1.13 — RTC·치트 입력·편집 데이터 보존

기본 과제: FS-14, FS-15, NP-19, NP-20, NP-21. 공동·후속: CJ-20.

소스 시작점: [RTC.cpp](../src/RTC.cpp), [ARDatabaseDAT.cpp](../src/ARDatabaseDAT.cpp), [AREngine.cpp](../src/AREngine.cpp), [ARCodeFile.cpp](../src/ARCodeFile.cpp), [CheatsDialog.cpp](../src/frontend/qt_sdl/CheatsDialog.cpp), [EmuInstance.cpp](../src/frontend/qt_sdl/EmuInstance.cpp).

기존 회귀 시작점: `core-.*execution|savestate`.

1. 잘린 RTC·치트 DB, 빈/E0 부족 code·과도한 loop/copy, 편집 저장 실패와 12/24시간·윤일·host 시간 변경을 재현한다. guest 시간과 host 동기화의 우선순위를 확정한다.
2. RTC exact read·원자적 저장과 DB read/seek/길이 오류 전파를 적용한다. 정상 entry만 반환하고 잘못된 code는 guest 실행 전에 거부한다.
3. 치트 실행 범위·취소/예산 계약을 정상 코드 표본에 맞춰 정하고 편집 실패를 UI로 전달한다. 코어의 Qt 비의존성을 유지하며 지원하지 않는 opcode를 명시한다.
4. 정상 조건/반복/활성 토글과 손상 입력·저장 거부에서 원본 보존을 확인한다. 날짜 의존 게임·IRQ pulse·DS/DSi 실기와 실제 치트 적용/해제는 별도 수락 사례다.
5. RTC 복원·다중 인스턴스 파일 정책과 치트 지원 범위를 문서화한다. 작은 고정 실행 상한으로 정상 치트를 끊거나 RTC IRQ를 근거 없이 묶지 않는다.

FS-15 편집 사본·직렬화·원자적 저장과 실패 복구는 [1.1.12 증분](releases/1.1.12.md)에 선반영한다. RTC·치트 실행 의미 및 실제 게임 수락 전체를 이 증분의 성공으로 완료 처리하지 않는다.

[1.1.13 증분](releases/1.1.13.md)은 NP-21의 반복마다 조건 복원과 이전 조건 없는 ENDIF 경계를 다룬다. C5 수명 등 미확정 동작은 정답을 추측하지 않고 원 핸들러·실기 수락 항목으로 유지한다.

<a id="v14"></a>

## 1.1.14 — x64 JIT 명령·플래그·예외 정확성

기본 과제: CJ-03, CJ-06.

소스 시작점: [ARMJIT_ALU.cpp](../src/ARMJIT_x64/ARMJIT_ALU.cpp), [ARMJIT_LoadStore.cpp](../src/ARMJIT_x64/ARMJIT_LoadStore.cpp), [ARMInterpreter_ALU.cpp](../src/ARMInterpreter_ALU.cpp), [CoreExecution.cpp](../tests/CoreExecution.cpp).

기존 회귀 시작점: `core-.*execution|arm-clz|jit-setting`.

1. ALU/shift/multiply/load-store의 interpreter와 JIT 차이 후보를 opcode·CPSR·alias·조건·PC 값으로 명시한다.
2. 실제 guest snippet으로 재현되는 결과·carry/overflow·PC/Thumb 전환·exception 차이를 우선 수정한다.
3. register cache·condition 실행·fastmem/helper 경로와 block re-entry를 연결한다. 단순 명령 교체와 cycle 수정을 분리한다.
4. shift 0/31/32/33/255·signed overflow·empty/list alias·조건 불성립·self-modify를 interpreter/일반 JIT/fastmem에서 비교한다.
5. 후속 branch/IRQ·save/load·두 게임의 영향 구간을 확인해 배포한다. interpreter와 일치해도 실기 근거가 없는 cycle은 미확인으로 기록한다.

[1.1.15 증분](releases/1.1.15.md)은 CJ-03의 x64 ASR 종료·Thumb 내부 사이클 누락을 수정하고 ARM9/ARM7 실제 guest ALU·shift·부분 flags와 두 게임의 전후 결과를 확인한다. CJ-03 전체와 CJ-06은 부분 진행이다. 릴리스 번호 1.1.15를 아래 메모리·CP15 책임 과제의 완료로 집계하지 않는다.

[1.1.16 증분](releases/1.1.16.md)은 CJ-06의 ARM/Thumb LDM/STM 빈 목록에 CPU별 전송·writeback·상태 복원과 기존 JIT fallback을 연결한다. 일반 ARM7 STM PC값과 ARM/Thumb 전환의 branch-follow 오류도 수정한다. 빈 PUSH/POP·실기 타이밍·native A64 수락은 남긴다. 실제 1.1.16 배포를 아래 OpenGL 책임 과제의 완료로 집계하지 않는다.

<a id="v15"></a>

## 1.1.15 — 메모리·CP15·DMA·IRQ·타이머

기본 과제: CJ-04, CJ-05, CJ-07, CJ-08, CJ-12, CJ-13, CJ-15. 공동·후속: CJ-03, CJ-06.

소스 시작점: [CP15.cpp](../src/CP15.cpp), [DMA.cpp](../src/DMA.cpp), [ARM.cpp](../src/ARM.cpp), [NDS.cpp](../src/NDS.cpp), [ARMJIT_Memory.cpp](../src/ARMJIT_Memory.cpp).

기존 회귀 시작점: `core-.*execution|bitfield|buffer-`.

1. TCM/mapping 변경·DMA fixed/decrement·IRQ 동시점·timer overflow의 관찰 가능한 결과를 작은 guest 프로그램으로 정의한다. DTCM oldEnd 계산, 금지 MPU 페이지 fall-through, due callback이 다음 event를 취소하는 순서를 먼저 재현한다.
2. 명세·실기 근거가 있는 주소/권한/우선순위/시계열 차이를 고친다. TODO만 보고 추정 delay를 추가하지 않는다.
3. JIT invalidation과 code/data fetch, DMA 중 CPU stall, scheduler 재등록·reset을 연결한다.
4. interpreter/JIT 메모리 trace·cycle·IRQ 순서를 비교하고, 외부 RAM/slot/TCM 경계의 실기 결과를 회수한다.
5. 장치로 확인된 정확도 범위를 기록하고 영향 게임·save/load를 통과한 변경만 배포한다. 전면 cycle 정확도라는 표현은 사용하지 않는다.

<a id="v16"></a>

## 1.1.16 — OpenGL context·캡처·자원 수명

기본 과제: CJ-19, GR-01, GR-04, GR-05, GR-06, GR-11, GR-12. 공동·후속: GR-03, GR-08, GR-13.

소스 시작점: [GPU_OpenGL.cpp](../src/GPU_OpenGL.cpp), [GPU2D_OpenGL.cpp](../src/GPU2D_OpenGL.cpp), [GPU3D_OpenGL.cpp](../src/GPU3D_OpenGL.cpp), [context.cpp](../src/frontend/graphics/gl/context.cpp), [GLFrameReadback.cpp](../tests/GLFrameReadback.cpp).

기존 회귀 시작점: `gpu-opengl-frame-capture|capture-readback`.

1. context 생성·공유·재생성·크기 변경·capture source A/B의 자원 소유권과 guest-visible 시점을 기록한다.
2. 실제 driver로 재현되는 context/FBO/texture 수명·오류 경로를 수정한다. 모든 재생성에서 기존 옵션이 복원되게 한다. pending capture를 보존한 배율 전환과 buffer/program 삭제 비대칭·초기 ID=0 경계를 우선 검증한다.
3. 3D source A→capture→VRAM→CPU/DMA 읽기와 2D compositor의 producer/consumer를 연결한다. CPU 요구 뒤로 readback을 미루지 않는다.
4. resize·renderer 변경·창 닫기·반복 생성·capture wrap과 source A/B를 NVIDIA에서 검사하고 AMD/Intel 결과를 별도 수집한다.
5. driver별 범위와 미지원 fallback을 문서화한다. 실제 GPU 객체 누수·잘못된 capture·종료 crash가 남으면 승격하지 않는다.

<a id="v17"></a>

## 1.1.17 — Compute renderer 정확성·동기화

기본 과제: GR-02, GR-03, GR-07. 공동·후속: GR-01, GR-06, GR-08.

소스 시작점: [GPU3D_Compute.cpp](../src/GPU3D_Compute.cpp), [GPU3D_Compute_shaders.h](../src/GPU3D_Compute_shaders.h), [GPU2D_OpenGL.cpp](../src/GPU2D_OpenGL.cpp), [GLFrameReadback.cpp](../tests/GLFrameReadback.cpp).

기존 회귀 시작점: `gpu-compute-frame-capture|capture-readback`.

1. 현재 1x 좌표·RenderSettingsDirty·sampler barrier 회귀를 유지하며 indirect dispatch·buffer/image 소비 관계를 명세와 대조한다. 같은 capture texture의 sampler만 바뀔 때 unit 선택, 3.2-only context·shader 실패 및 높은 배율 할당 실패를 재현한다.
2. polygon/texture/clear/depth/fog 경계에서 필요한 동기화와 좌표·정수 계산만 수정한다. 무조건 glFinish를 추가하지 않는다. feature 부족·shader 실패를 선택기와 fallback으로 전파하고 sampler unit을 variant별로 결정한다.
3. 옵션 변경·shader 재컴파일·동일 프레임 재사용·capture와 연결하고 cache key/객체 수명을 확인한다.
4. 1x/2x 이상·정지 화면·연속 변경·실제 게임·여러 GPU에서 pixel/capture 결과와 API 오류를 검사한다.
5. 실제 driver별 정확성·재시작·장기 자원 사용을 확인한 범위만 안정화한다. 명세상 수정과 특정 driver의 화면 재현을 구분한다.

<a id="v18"></a>

## 1.1.18 — DSi 부팅·firmware·원본 대조

기본 과제: CJ-21, AD-09, AD-10, AD-17, FS-19. 공동·후속: AD-13.

소스 시작점: [DSi.cpp](../src/DSi.cpp), [DSi.h](../src/DSi.h), [EmuInstance.cpp](../src/frontend/qt_sdl/EmuInstance.cpp), [ROMSmoke.cpp](../tests/ROMSmoke.cpp).

기존 회귀 시작점: `firmware-profile|core-.*execution`.

1. 현재 두 게임 실패의 direct/menu boot·DS/DSi·region·BIOS role·firmware·SCFG 차이를 동일 자료와 설정으로 upstream 기준과 비교한다. modcrypt dev key의 미초기화 tmp와 내부 subarea offset은 합성 header/key vector로 원본 부팅과 독립 재현한다.
2. 소스 trace와 재현 근거가 있는 boot/SCFG/메모리 초기화·role 검증 결함을 고친다. 파일 이름만으로 BIOS 종류를 추정하지 않는다. modcrypt key byte 순서와 subarea RAM 시작 주소를 독립 vector로 판정한다.
3. DS 모드 기본값과 기존 Profile를 보존하면서 DSi 실패 진단·boot 선택·reset/load를 연결한다.
4. 합성 homebrew·한국 메뉴·게임별 direct/menu 경로를 확인한다. 원본에서도 동일한지, 패치/region 영향인지 별도 결과를 기록한다.
5. 검증한 boot 조합만 명시한다. DSi 전용/확장 소프트웨어와 장치 결과가 없으면 DSi 전체 experimental 표시를 제거하지 않는다.

<a id="v19"></a>

## 1.1.19 — DSi NAND·SD·파일 시스템 복구

기본 과제: AD-11, AD-12, AD-13, FS-16. 공동·후속: AD-15.

소스 시작점: [DSi_NAND.cpp](../src/DSi_NAND.cpp), [DSi_SD.cpp](../src/DSi_SD.cpp), [DSi_SD.h](../src/DSi_SD.h), [EmuInstance.cpp](../src/frontend/qt_sdl/EmuInstance.cpp).

기존 회귀 시작점: `savestate-|buffer-|core-.*execution`.

1. NAND/SD 명령·image 크기·seek/read/write·write-protect·분리·오류 전달의 실제 흐름과 소유권을 기록한다. SCR/SSR의 producer 길이와 BlockLen16 불일치, sector 끝을 넘는 부분 전송, 잘린 HWINFO와 같은 title ID 교체 실패를 포함한다.
2. 길이·부분 I/O·경계 주소·상태 전이의 정적 의심을 합성 image로 재현하고 수정한다. 원본 보존에는 우선 폐기 가능한 합성 이미지·분리된 시험 사본을 사용한다. 새 copy-on-write overlay의 구현은 이 수정의 필수 조건으로 넣지 않고 미래 계획 검토로 분리한다. backing I/O 및 NAND metadata exact read 실패를 caller까지 전달하고 title 교체는 삭제 전 staging/복원 경계를 정한다.
3. FIFO/DMA·save/load·카드 교체·재시작과 image state를 연결한다. host 파일 오류와 guest 장치 오류를 구분한다.
4. read/write/erase·끝 sector·저장 부족/분리 오류·state 복원을 합성 image와 분리된 시험 사본으로 확인한다. overlay discard/commit은 별도 overlay 기능을 채택한 경우에만 그 기능의 gate에서 검사한다.
5. 전원 손실/실제 장치 검증의 범위를 분리하고 원본 보존·재열기를 확인해 배포한다. 임의 NAND 수정이나 데이터 자동 전송은 하지 않는다.

<a id="v20"></a>

## 1.1.20 — DSi SCFG·NWRAM·NDMA·reset 타이밍

기본 과제: CJ-14, AD-15, AD-16. 공동·후속: AD-17.

소스 시작점: [DSi.cpp](../src/DSi.cpp), [DSi_NDMA.cpp](../src/DSi_NDMA.cpp), [DSi_DSP.cpp](../src/DSi_DSP.cpp), [DSi.h](../src/DSi.h).

기존 회귀 시작점: `core-.*execution|teakra-execution`.

1. 겹치는 NWRAM slot·SCFG 변경·NDMA channel priority/subblock·reset/power 전이의 현재 근사치를 목록화한다. AES 즉시 처리·SD의 고정 완료 delay와 NDMA subblock/IRQ의 공통 event 계약을 장치별로 나눈다.
2. 동일 tick의 CPU/DMA/DSP 관찰 순서와 주소 wrap·slot 우선순위를 합성 프로그램/실기 oracle로 확인한 범위부터 구현한다.
3. mapping 변경의 JIT invalidation·DSP 접근·IRQ 재예약·save/load와 연결한다. reset 중 보존되는 RAM과 초기화되는 레지스터를 구분한다.
4. overlap/out-of-range·고정/증감 주소·여러 DMA·중간 reset·state 복원·권한 변경을 비교한다. 시간 관계는 실제 DSi 자료를 필요 조건으로 둔다.
5. 확인되지 않은 delay/우선순위를 임의 상수로 채워 안정 승격하지 않는다. 확인된 조합과 실기 대기 항목을 표시하고 배포한다.

<a id="v21"></a>

## 1.1.21 — DSP modulo·메모리·플래그 결함

기본 과제: AD-18.

소스 시작점: [interpreter.h](../src/teakra/src/interpreter.h), [test.h](../src/teakra/src/test.h), [DSi_DSP.cpp](../src/DSi_DSP.cpp), [main.cpp](../src/teakra/src/test_verifier/main.cpp).

기존 회귀 시작점: `teakra-execution`.

1. 기존 54,288번 실패와 관련 147/148 결과·254개 실기 입력을 고정하고 예상 출력과 입력을 구분한다.
2. DSP 검증 기기의 DSP 시험에서 새 출력과 장치/시험 ID를 회수한다. modulo -1의 두 주소를 구분하는 일반 규칙을 도출한다.
3. 근거가 확보된 주소/flag 규칙을 interpreter·external memory callback·reset 경계에 적용하고 정상 인접 사례를 유지한다.
4. 단일 실패→관련 표본→영향 명령군 순으로 검증하고 전체 corpus는 필요한 최종 검증 한 번만 수행한다. skip·불완전 record를 통과시키지 않는다.
5. 하드웨어 규칙과 회귀가 모두 맞으면 배포한다. oracle가 없거나 규칙이 충돌하면 실패 1건을 열린 상태로 유지하고 과적합 패치를 넣지 않는다.

<a id="v22"></a>

## 1.1.22 — DSP 전체 명령·이벤트·타이밍 검증

기본 과제: AD-19, AD-20, AD-21, AD-26. 공동·후속: AD-01, AD-02.

소스 시작점: [src](../src/teakra/src), [DSi_DSP.cpp](../src/DSi_DSP.cpp), [TeakraExecution.cpp](../tests/TeakraExecution.cpp), [main.cpp](../src/teakra/src/test_verifier/main.cpp).

기존 회귀 시작점: `teakra-execution`.

1. opcode만 아니라 operand/flag/address/event/timing의 coverage matrix를 작성하고 미검증 조합을 분류한다.
2. interrupt·FIFO·DMA·sleep/reset·repeat/loop 경계와 cycle 모델의 차이를 공개 문서/실기 입력으로 확인한다. retd 내부 실행·vtrshr 지연, PSTS FIFO/PCFG mask·HLE/LLE 응답·I2S overrun을 독립 시험군으로 둔다.
3. 확인된 이벤트·메모리 타이밍을 DSi scheduler와 연결하고 state serialization 및 debugger 관찰과 일치시킨다.
4. 명령군별 최소 경계·실기 trace·DSi audio workload로 검증한다. 기능 corpus의 일치를 cycle 정확도 검증으로 대신하지 않는다.
5. 지원/검증 범위를 공개하고 반복 가능한 corpus와 입력 생성법을 제공한다. DSP JIT는 이 기준을 선행하는 별도 Future 연구로 남긴다.

<a id="v23"></a>

## 1.1.23 — DSi 카메라·I2C·전원 주변장치

기본 과제: AD-22, AD-23, FS-21. 공동·후속: AD-21.

소스 시작점: [DSi_Camera.cpp](../src/DSi_Camera.cpp), [DSi_I2C.cpp](../src/DSi_I2C.cpp), [CameraManager.cpp](../src/frontend/qt_sdl/CameraManager.cpp), [CameraSettingsDialog.cpp](../src/frontend/qt_sdl/CameraSettingsDialog.cpp).

기존 회귀 시작점: `core-.*execution|savestate-`.

1. camera register·YUV/format/crop·buffer priority·I2C 장치·lid/power의 근사 부분과 host device 수명을 분리한다.
2. 합성 camera source와 작은 register sequence로 pixel/stride/format·초기화/종료 오류를 재현하고 수정한다.
3. camera DMA/FIFO·NDMA·사용 시작/종료·권한 실패·장치 교체·save/load를 연결하고 사용하지 않는 카메라는 해제한다.
4. 합성 패턴과 실제 camera·DSi 소프트웨어·실기의 frame/crop/IRQ/전원 전이를 확인한다. 녹화·전송은 기본 동작에 추가하지 않는다.
5. 검증된 기능·format과 장치 실패 복구를 명시해 배포한다. 일부 사진 앱 실행만으로 DSi 주변장치 전체 완료를 선언하지 않는다.

<a id="v24"></a>

## 1.1.24 — 카트리지·homebrew·GBA 슬롯 주변장치

기본 과제: NP-13, NP-14, NP-15, NP-16, NP-17, NP-18.

소스 시작점: [NDSCart.cpp](../src/NDSCart.cpp), [CartRetailIR.cpp](../src/NDSCart/CartRetailIR.cpp), [CartRetailBT.cpp](../src/NDSCart/CartRetailBT.cpp), [CartHomebrew.cpp](../src/NDSCart/CartHomebrew.cpp), [CartSD.cpp](../src/NDSCart/CartSD.cpp), [GBACart.cpp](../src/GBACart.cpp), [GBACartMotionPak.cpp](../src/GBACartMotionPak.cpp).

기존 회귀 시작점: `gba-rom|nds-rom|core-.*execution`.

1. IR 미지원 명령/중간 복원, 긴 homebrew argv와 DLDI tail signature, 미등록 save type, 슬롯 소유 변경을 분리한다. 실제 카드·키보드·센서가 필요한 기대 응답은 추측하지 않는다.
2. 도달 가능한 argv/DLDI 입력 경계와 IR 초기화·상태 저장을 먼저 보정한다. 기존 카트리지 header 방어·정상 save 형식과 callback 수명을 유지한다.
3. 실제 trace를 얻은 BT 키보드 초기화/키 해제 흐름과 슬롯 IRQ/DRQ 전이를 구현한다. 태양광·모션·진동·RAM 확장은 장치별 계약과 실패한 조합만 수정한다.
4. 합성 parser·전송/복원 회귀 후 해당 게임에서 SD 읽기/쓰기, DS↔GBA 연동, 장치 교체·pause/eject 시 진동 중지를 확인한다. 실물 카드/센서 결과는 별도 장치 gate다.
5. 지원 장치·미지원 command·save type의 모호성을 명시한다. 자동 저장 변환이나 실기 근거 없는 응답을 기본값으로 승격하지 않고 검증된 장치 단위로 공개한다.

<a id="v25"></a>

## 1.1.25 — BMI·block cache·JIT 성능

기본 과제: CJ-17, CJ-18.

소스 시작점: [ARMJIT_ALU.cpp](../src/ARMJIT_x64/ARMJIT_ALU.cpp), [ARMJIT.cpp](../src/ARMJIT.cpp), [ARMJIT_RegisterCache.h](../src/ARMJIT_RegisterCache.h), [CPUDetect.h](../src/jit/CPUDetect.h).

기존 회귀 시작점: `core-.*execution|jit-setting`.

1. JIT compile/execute 비중·block 크기·cache hit 분포를 측정하고 ANDN·BMI2 shift의 실제 적용 구간을 고른다.
2. S=0 등 좁은 BIC/shift 경로부터 CPU feature gate와 fallback을 구현한다. ARM low8 shift·0/32/>32와 guest C/V를 보존한다.
3. block hash/identity·invalidation·메모리 예산의 병목만 개선한다. 캐시 알고리즘과 ISA 변경의 효과를 별도로 측정한다.
4. 현재 x64 guest corpus와 강제 no-BMI·no-LZCNT 경로를 비교한다. compile 지연을 포함한 kernel/대표 게임/holdout 결과를 확인한다.
5. 다른 CPU에서 느린 PDEP/PEXT를 포괄 활성화하지 않는다. 이득이 측정된 선택만 배포하고 불확실한 적응 재컴파일은 연구로 남긴다.

<a id="v26"></a>

## 1.1.26 — ISA 분기·CRC·hash·공통 커널

기본 과제: BV-12. 공동·후속: CJ-17, CJ-18.

소스 시작점: [HostFeatures.cmake](../cmake/HostFeatures.cmake), [PixelConvert.cpp](../src/PixelConvert.cpp), [CRC32.cpp](../src/CRC32.cpp), [sha1.c](../src/sha1/sha1.c), [xxhash.h](../src/xxhash/xxhash.h).

기존 회귀 시작점: `pixel-conversion|standard-bit|aes-byteswap`.

1. HostISA의 후보를 실제 호출 크기·빈도로 대조한다. CRC polynomial·hash algorithm·seed·null buffer 계약을 고정한다.
2. 필요한 CRC/XXH3/byte 처리만 기존 검증된 backend로 연결한다. tiny loop의 dispatch 비용이 이득보다 크면 유지한다.
3. 커널별 CPU/OS capability와 explicit fallback을 제공한다. 표준 SIMD 후보는 실제 compiler/STL probe를 통과할 때 별도 구현으로 비교한다.
4. length 0·짧은/tail·unaligned·page 끝·CRC chaining·XXH3와 XXH64 분리 및 unsupported feature negative control을 확인한다.
5. 정확한 digest와 대표 block 크기 이득이 있는 구현만 배포한다. AES/SHA 등의 전체 기능을 사용하지 않았는데 모두 가속했다고 표시하지 않는다.

<a id="v27"></a>

## 1.1.27 — 소프트웨어 2D 합성·밝기 SIMD

기본 과제: GR-09. 공동·후속: GR-08.

소스 시작점: [GPU2D_Soft.cpp](../src/GPU2D_Soft.cpp), [GPU_ColorOp.h](../src/GPU_ColorOp.h), [GPU_Soft.cpp](../src/GPU_Soft.cpp), [PixelConvert.cpp](../tests/PixelConvert.cpp).

기존 회귀 시작점: `pixel-conversion|software-capture`.

1. ColorComposite·master brightness의 연속 구간을 측정하고 RGB6·alpha·window·OBJ 우선순위·bias를 고정한다.
2. 페이즈 1에서 유효한 병목 후보가 확인된 경우에만 AVX2와 baseline 정수 커널을 같은 함수 경계에서 비교 구현한다. 후보가 없으면 기존 경로를 유지한다. widen multiply→bias→shift→clamp63 순서를 보존한다.
3. 배율·capture·layer mask·3D 혼합·tail에 기존 scalar 경로를 연결한다. ISA dispatch는 scanline/블록 밖에서 수행한다.
4. 채널 0/1/62/63, 계수 경계, window/OBJ/3D 조합과 정렬·tail을 scalar 및 합성 화면과 비교한다.
5. 대표 2D 장면에서 이득을 확인한 후 배포한다. AVX-512/NEON은 같은 계약으로 추가하며 미지원 CPU에서 scalar 결과가 유지돼야 한다.

<a id="v28"></a>

## 1.1.28 — 소프트웨어 3D fog·AA·texture 처리

기본 과제: GR-08, GR-10, GR-13. 공동·후속: GR-09.

소스 시작점: [GPU3D_Soft.cpp](../src/GPU3D_Soft.cpp), [GPU3D_Texcache.cpp](../src/GPU3D_Texcache.cpp), [GPU3D_Texcache.h](../src/GPU3D_Texcache.h).

기존 회귀 시작점: `core-.*execution|pixel-conversion`.

1. ScanlineFinalPass의 edge→fog→AA 순서, top/lower pixel, u32 wraparound, coverage와 texture cache hit 비용을 기록한다. software worker의 thread 전환·scanline 공개·중단/join과 texture cache high-water를 관측한다. 상주 pool을 바로 누수로 단정하지 않는다.
2. 확인된 depth/fog/AA/worker 수명 결함은 최소 수정으로 처리한다. 페이즈 1에서 실제 병목이 확인된 경우에만 연속 final pass의 정수 compare/mask/widen 연산을 좁게 SIMD화한다. gather·polygon 전체 벡터화는 별도 실험으로 둔다.
3. depth·alpha-only fog·shadow·texture format·cache 무효화와 연결하고 source별 정수 연산을 무리하게 공통화하지 않는다.
4. depth/shift overflow·fog table 끝·coverage0/31·투명 texture·VRAM 변경·실제 3D 장면을 기준 renderer와 비교한다.
5. 정확성·worker 수명 수정은 재현·회귀·관련 장치 gate를 통과한 범위로 배포하며 FPS 향상을 필수로 요구하지 않는다. 선택적 성능 변경만 같은 정확성 모드의 frame time 이득을 입증해야 채택한다. 고해상도/향상 옵션의 차이는 native 정확성 결과와 분리한다.

<a id="v29"></a>

## 1.1.29 — DSi AES·CTR/CCM·하드웨어 crypto

기본 과제: AD-14, AD-24. 공동·후속: AD-09, AD-15.

소스 시작점: [DSi_AES.cpp](../src/DSi_AES.cpp), [aes.c](../src/tiny-AES-c/aes.c), [DSi_NAND.cpp](../src/DSi_NAND.cpp).

기존 회귀 시작점: `aes-byteswap|core-.*execution`.

1. AES round/key/byte layout, CTR carry·CCM CBC-MAC 의존, FIFO/DMA/IRQ cycle 계약을 고정한다.
2. 불완전 mode·error/timing 처리를 공개 vector와 합성 register sequence로 확인하고 수정한다.
3. AES block 비용이 유효한 병목으로 측정된 경우에만 AES-NI 또는 ARM AES backend를 기존 primitive 경계에 연결한다. VAES는 독립 block이 충분할 때만 별도 실험하며 PCLMUL을 CCM 필수로 묶지 않는다.
4. NIST vector·unaligned/in-place·key 변경·CTR 경계·MAC·save/load·FIFO sequence를 scalar와 비교한다. 합성 key만 문서에 사용한다.
5. AES 프로토콜·오류·타이밍의 정확성 수정은 각 vector와 장치 gate로 판정한다. 선택적 가속 backend는 DSi 기능 동등성과 해당 호출량에서의 이득이 함께 입증된 경우에만 활성화한다. ISA 도입을 원래 AES 기능·타이밍 완성의 대체 증거로 쓰지 않는다.

<a id="v30"></a>

## 1.1.30 — 표시 지연·프레임 pacing·전송 비용

기본 과제: GR-14. 공동·후속: BV-10, GR-11.

소스 시작점: [Screen.cpp](../src/frontend/qt_sdl/Screen.cpp), [EmuThread.cpp](../src/frontend/qt_sdl/EmuThread.cpp), [GPU_OpenGL.cpp](../src/GPU_OpenGL.cpp), [graphics](../src/frontend/graphics).

기존 회귀 시작점: `gpu-.*frame-capture|audio-core-clock`.

1. CPU emulation·render·upload·readback·present·audio wait를 분리 계측하고 입력→화면 경로의 현재 queue 깊이를 고정한다.
2. guest와 독립적인 표시/업로드에만 ring buffer·dirty region·불필요 대기 제거를 실험한다. guest capture의 동기 요구는 유지한다.
3. VSync·고정/가변 refresh·speed limiter·pause/frame advance·다중 창과 연결한다.
4. 표시 frame 간격·누락/중복·audio drift·p95/p99·실제 입력 지연을 비교한다. microbenchmark 이득과 전체 지연을 구분한다.
5. 최소 지연과 안정 표시 preset을 설명하고 default는 실제 장치 회귀가 없는 조합으로 정한다. 불확실한 비동기 경로는 opt-in으로 유지한다.

<a id="v31"></a>

## 1.1.31 — Netplay의 실제 게임 경로 완성

기본 과제: NP-09. 공동·후속: FS-17, CJ-20.

소스 시작점: [Netplay.cpp](../src/net/Netplay.cpp), [NetplayDialog.cpp](../src/frontend/qt_sdl/NetplayDialog.cpp), [Window.cpp](../src/frontend/qt_sdl/Window.cpp).

기존 회귀 시작점: `network|savestate-roundtrip|core-.*execution`.

1. 기존 비활성 mirror/blob 흐름을 근거로 지원 topology·동기화 대상·동일 게임/설정/상태 조건을 확정한다. 네트워크 입력과 저장·RTC 등 외부 부작용의 소유자를 정한다.
2. 최소 host/client 한 흐름의 연결·상태 동기화·입력 전달·중도 종료를 연결한다. 버전/게임 불일치와 동기화 실패를 게임 실행 전에 판별한다.
3. 세션 수명·mirror 종료·UI 취소·disconnect 복구를 완성하고 실험 옵션으로 노출한다. 진행 중 desync는 조용히 무시하지 않고 원인 비교 자료를 남긴다.
4. 합성 왕복 후 서로 다른 PC에서 실제 게임을 한 세션 끝까지 진행한다. 손실·지연·clock 차이·pause·저장·재참가와 CPU/renderer 조합의 결정성을 확인한다.
5. 지원 topology·게임·host 조합과 지연/대역폭 측정을 공개한다. 재현 가능한 desync나 종료 실패가 남으면 실험 상태를 유지하고 안정 LAN을 계속 선택할 수 있게 한다.

<a id="v32"></a>

## 1.1.32 — Vulkan 장치·창·기본 표시

기본 과제: GR-15, GR-16.

소스 시작점: [GPU.h](../src/GPU.h), [Screen.cpp](../src/frontend/qt_sdl/Screen.cpp), [graphics](../src/frontend/graphics), [CMakeLists.txt](../src/CMakeLists.txt).

기존 회귀 시작점: `gpu-.*frame-capture`.

1. 기존 renderer interface에서 표시 texture·guest capture·shader compile의 계약을 추출한다. Vulkan 요구 버전/기능과 GL/Software fallback을 고정한다.
2. 새 기능별 파일 src/GPU_Vulkan.* 및 frontend/graphics의 Vulkan context 계층을 제안 위치로 만들고 instance/device/queue/swapchain의 최소 표시를 구현한다.
3. Qt 창 수명·resize·최소화·장치 선택·실패 복귀·설정 저장을 연결한다. 다른 프로젝트 이름의 소스 폴더는 만들지 않는다.
4. 합성 두 화면의 색·stride·rotation과 swapchain out-of-date·실패 정리를 확인한다. 새 검사는 기존 GPU fixture 방식으로 추가한다.
5. 사용 가능한 기본 표시와 정상 fallback을 가진 experimental backend로 배포한다. DS 3D/캡처 완성을 주장하지 않고 다음 버전 범위를 표시한다.

<a id="v33"></a>

## 1.1.33 — Vulkan 3D rasterization

앞 버전 과제의 연속 단계. 공동·후속: GR-15, GR-16, GR-08.

소스 시작점: [GPU3D_Soft.cpp](../src/GPU3D_Soft.cpp), [GPU3D_Compute.cpp](../src/GPU3D_Compute.cpp), [GPU3D_Texcache.h](../src/GPU3D_Texcache.h), [GPU.h](../src/GPU.h).

기존 회귀 시작점: `gpu-.*frame-capture`.

1. DS 3D의 vertex/polygon/texture/depth/shadow/fog/AA 의미를 기준 구현과 연결하고 shared state와 backend 전용 자원을 분리한다.
2. 새 GPU3D_Vulkan 계층에 명확한 기본 polygon·texture·depth 경로를 구현하고 shader 입력/출력 포맷을 고정한다.
3. alpha blend·shadow·edge/fog/AA·palette/texture invalidation·배율 설정을 단계적으로 연결한다. 비지원 명령은 진단 가능하게 처리한다.
4. 각 효과의 작은 장면과 실제 3D 게임에서 native framebuffer·depth 관련 상태를 Software/Compute 및 실기 기준과 비교한다.
5. 대상 3D 기능이 실행·재설정·save/load에서 유지되는 experimental 버전을 배포한다. 누락된 DS 효과를 숨긴 채 Vulkan 완성으로 표시하지 않는다.

<a id="v34"></a>

## 1.1.34 — Vulkan 2D·display capture·CPU readback

앞 버전 과제의 연속 단계. 공동·후속: GR-15, GR-16, GR-05, GR-06.

소스 시작점: [GPU2D_Soft.cpp](../src/GPU2D_Soft.cpp), [GPU2D_OpenGL.cpp](../src/GPU2D_OpenGL.cpp), [GPU_OpenGL.cpp](../src/GPU_OpenGL.cpp), [GPU.h](../src/GPU.h).

기존 회귀 시작점: `capture-readback|software-capture|gpu-.*frame-capture`.

1. 2D layer/window/OBJ/3D 혼합·master brightness·source A/B capture·128/256폭 wrap의 observable 계약을 고정한다.
2. 새 GPU2D_Vulkan 및 Vulkan capture 경로를 구현한다. 동기화는 실제 후속 접근 유형과 resource 상태로 결정한다.
3. capture 후 guest CPU/DMA 읽기, renderer 교체·save/load·static frame·dirty range와 연결한다.
4. 실제 GPU→capture→VRAM→CPU의 byte 결과와 다중 범위·동시 frame 변경·integer bit 확장을 기준 구현과 비교한다.
5. DS 화면과 capture를 포함한 end-to-end 동작을 확인해 배포한다. 캡처가 불완전하면 전용 호환성 경고와 fallback을 유지한다.

<a id="v35"></a>

## 1.1.35 — Vulkan cache·장치 손실·안정 승격

앞 버전 과제의 연속 단계. 공동·후속: GR-15, GR-16, BV-19.

소스 시작점: [graphics](../src/frontend/graphics), [GPU.h](../src/GPU.h), [VideoSettingsDialog.ui](../src/frontend/qt_sdl/VideoSettingsDialog.ui), [EmuInstance.cpp](../src/frontend/qt_sdl/EmuInstance.cpp).

기존 회귀 시작점: `gpu-.*frame-capture`.

1. shader/pipeline cache 키·descriptor/buffer/image 수명·메모리 예산·장치 손실 동작을 정한다.
2. cache 버전/driver/옵션 무효화와 제한된 pipeline 재사용·전송 batch를 구현하고 비동기 compile의 취소 경계를 고정한다.
3. 기기 재생성·프로그램 종료·fallback·설정 UI·배포 loader/shader 자산과 연결한다. backend별 내부 차이는 필요할 때만 노출한다.
4. cold/warm startup·여러 GPU·장기 게임·resize/minimize·device-loss 오류 주입과 실제 장치 복구를 확인한다. 같은 정확성/해상도의 GL/Compute와 비교한다.
5. 정확성·복구·게임·지원 GPU gate가 모두 찬 범위만 안정 승격한다. GL보다 느린 장치에 Vulkan을 자동 강제하지 않는다.

<a id="v36"></a>

## 1.1.36 — 효용 중심 C23·C++26·의존성 갱신

기본 과제: BV-06, BV-07. 공동·후속: BV-03, BV-04.

소스 시작점: [CMakeLists.txt](../CMakeLists.txt), [DefaultBuildFlags.cmake](../cmake/DefaultBuildFlags.cmake), [vcpkg.json](../vcpkg.json), [overlay-ports](../cmake/overlay-ports).

기존 회귀 시작점: `cxx26|c23|standard-bit|bitfield|utf8`.

1. 현재 dialect·실제 사용 기능·STL/컴파일러 지원과 vendored/local patch를 분리한 표를 만든다. 과거 C11·구버전 패키지 기록을 현재 사실처럼 섞지 않는다. 의존성 검증에 필요한 최소 DLL 확인은 여기서 직접 수행하고 후속 배포 도구 완성 전체를 기다리지 않는다.
2. 오류 제거·복사/할당 감소·컴파일 시간·표현 간결화 중 이득이 있는 변경만 고른다. 기능 시험 macro·최소 지원 compiler·fallback으로 채택 조건을 정의한다.
3. 작은 기능 단위로 표준 API를 적용하고 의존성은 호환 API 묶음별로 갱신한다. Qt/SDL 대전환은 별도 버전으로 두고 third-party 코드를 장식적 스타일 변경하지 않는다.
4. 해당 기존 회귀와 Windows 지원 toolchain으로 affected target을 확인한다. ABI·파일/상태 형식·signed/unsigned·정수 wrap·header self-containment를 변경 범위에 맞게 확인한다.
5. 사용한 기능과 실제 이득, 유지한 fallback·지원 버전·local patch·license를 기록한다. C++26 모드 설정만으로 전체 표준 기능 완전 지원을 주장하지 않는다.

<a id="v37"></a>

## 1.1.37 — SDL3 전환과 장치 API 통합

기본 과제: BV-20. 공동·후속: FS-09, FS-21, AD-04.

소스 시작점: [CMakeLists.txt](../src/frontend/qt_sdl/CMakeLists.txt), [EmuInstanceAudio.cpp](../src/frontend/qt_sdl/EmuInstanceAudio.cpp), [EmuInstanceInput.cpp](../src/frontend/qt_sdl/EmuInstanceInput.cpp), [CameraManager.cpp](../src/frontend/qt_sdl/CameraManager.cpp), [vcpkg.json](../vcpkg.json).

기존 회귀 시작점: `qt-keyboard|audio-callback|audio-core`.

1. SDL2→3의 audio stream·gamepad·event·threading 변경을 실제 사용하는 API만 목록화한다. 현재 장치 재연결과 지연 기준을 고정하고 전환 이득을 확인한다.
2. 플랫폼 포장과 초기화/종료 경계부터 작은 adapter 또는 직접 API 전환 중 더 단순한 방식을 택한다. 영구적인 두 SDL backend 유지 비용을 피한다.
3. 입력·motion/rumble·마이크/출력·hotplug를 순차 이전하고 설정 migration·DLL 배포를 연결한다. SDL3 전환으로 DS guest sample clock을 바꾸지 않는다.
4. 현재 회귀와 실제 키/패드/오디오/마이크·sleep/resume·다중 인스턴스·포커스·종료를 비교한다. 평균과 tail 지연·underrun·CPU 비용을 같은 환경에서 측정한다.
5. 기능/장치 회귀 없이 유지 비용 또는 실제 품질 이득이 확인될 때 기본 전환한다. 미충족이면 연구 브랜치에 남기고 마지막 SDL2 빌드를 재현 가능하게 유지한다.

<a id="v38"></a>

## 1.1.38 — LTO·PGO·컴파일러 최적화

기본 과제: BV-08, BV-09. 공동·후속: BV-04, BV-10.

소스 시작점: [DefaultBuildFlags.cmake](../cmake/DefaultBuildFlags.cmake), [CMakeLists.txt](../CMakeLists.txt), [ci-current-build.py](../tools/ci-current-build.py).

기존 회귀 시작점: `core-.*execution|pixel-conversion|audio-core`.

1. GCC15+ LTO 비활성 guard의 실제 ICE 재현과 현재 compiler 결과를 분리한다. PGO train·검증용 holdout 게임/장면 및 JIT/renderer별 대표 비율을 고정한다.
2. LTO/ThinLTO 문제는 최소 재현에서 해결된 toolchain 조합만 허용한다. 전체 native/fast-math를 켜는 대신 재현 가능한 로컬 profile build 절차를 만든다.
3. 계측 build→학습→profile 병합→최적화 build를 연결하고 stale/missing profile을 진단한다. 사용하지 않는 게임 경로를 자동으로 저우선 처리해 호환성을 깨지 않는다.
4. holdout에서 상태/화면/PCM 정확성, 시작·compile·frame median/p95/p99·크기·빌드 비용을 비교한다. 이 변경의 full suite는 선택한 로컬 구성에서 한 번만 수행한다.
5. 이득이 반복되는 설정만 배포 기본값에 반영하고 profile provenance·fallback build를 남긴다. 학습 게임 한 장면의 수치를 전체 게임 향상으로 보고하지 않는다.

<a id="v39"></a>

## 1.1.39 — ARM64 JIT 정확성과 native 실행

기본 과제: CJ-01, CJ-02, CJ-16. 공동·후속: CJ-03, CJ-04, CJ-15.

소스 시작점: [ARMJIT_ALU.cpp](../src/ARMJIT_A64/ARMJIT_ALU.cpp), [ARMJIT_LoadStore.cpp](../src/ARMJIT_A64/ARMJIT_LoadStore.cpp), [ARMJIT_Compiler.cpp](../src/ARMJIT_A64/ARMJIT_Compiler.cpp), [ARM64JitArithmetic.cpp](../tests/ARM64JitArithmetic.cpp).

기존 회귀 시작점: `arm64-jit-arithmetic|core-.*execution`.

1. 기존 144개 A64 emitter 사례를 재사용하고 부분 flag liveness·shifted operand·PC·alias·multiply cycle의 빈 영역을 목록화한다. ARM7 MUL 가변 I 계산과 조건 불성립 C 누적 후보를 우선한다.
2. 제한된 ISA 모델로 재현 가능한 코드 생성 결함을 수정한다. scratch register와 캐시된 guest register의 충돌 조건을 확인한다.
3. 공통 CoreExecution guest 입력을 native A64 block 실행에 연결한다. W/X 폭·ABI·I-cache publication·fastmem fault 수명을 보존한다.
4. Windows/Linux ARM64에서 interpreter·JIT·fastmem 결과·cycle·후속 조건분기·재실행을 비교한다. macOS 수락은 별도 플랫폼 검증으로 남긴다.
5. native 장치 결과가 확보된 범위만 안정화한다. native 실행을 검증하지 못하면 생성 코드 수정과 native 미검증 상태를 분리한 실험 상태로 남긴다.

<a id="v40"></a>

## 1.1.40 — ARM64 native·NEON·기본 확장 검증

기본 과제: BV-15. 공동·후속: CJ-16, GR-09, AD-24.

소스 시작점: [ARMJIT_Compiler.cpp](../src/ARMJIT_A64/ARMJIT_Compiler.cpp), [ARMJIT_Memory.cpp](../src/ARMJIT_Memory.cpp), [HostFeatures.cmake](../cmake/HostFeatures.cmake), [PixelConvert.cpp](../src/PixelConvert.cpp).

기존 회귀 시작점: `arm64-jit|core-.*execution|pixel-conversion|aes-byteswap`.

1. 실제 Windows/Linux ARM64 native 실행 환경의 CPU feature·OS context·페이지 크기·compiler를 기록한다. cross build와 모델 실행은 별도 상태로 유지한다.
2. A64 JIT·fastmem·코드 게시/I-cache·MMIO fallback의 native 기준을 먼저 통과시킨다. 실패는 host 보호 정책을 완화해 우회하지 않는다.
3. 2D/3D 정수 후처리·기존 픽셀 변환·AES 등 측정된 커널에 NEON 및 해당 장치의 기본 확장을 적용한다. feature별 런타임 fallback과 scalar 경계를 재사용한다.
4. 실제 ARM64에서 tails/unaligned·overflow·flags/cycles·warmed JIT·게임 출력·오디오/입력과 backend 전환을 비교한다. CPU 온도/전력과 장기 clock 변화도 측정 조건에 남긴다.
5. 실행한 기기·OS·명령 경로만 지원 근거로 표시한다. Apple은 별도 조건부 버전으로 남기며 ARM64라는 이유로 모든 확장을 켜지 않는다.

<a id="v41"></a>

## 1.1.41 — 고급 ARM 확장·Apple 플랫폼 조건부 지원

기본 과제: BV-16. 공동·후속: BV-12.

소스 시작점: [HostFeatures.cmake](../cmake/HostFeatures.cmake), [Arm64Emitter.cpp](../src/jit/Arm64Emitter.cpp), [ARMJIT_Compiler.cpp](../src/ARMJIT_A64/ARMJIT_Compiler.cpp).

기존 회귀 시작점: `arm64-jit|core-.*execution|pixel-conversion`.

1. SVE/SVE2·SME/SME2·dot product·crypto 등 후보를 실제 consumer CPU·OS API·ABI·vector length·thread state 계약으로 분류한다. 제품 세대를 단일 feature 집합으로 가정하지 않는다.
2. 유효한 큰 batch와 data layout이 있는 커널만 prototype 대상으로 정한다. 스칼라 DS 제어 코드와 작은 작업에 matrix 명령을 억지 적용하지 않는다.
3. 지원 compiler·OS에서 허용된 feature dispatch와 안전한 fallback을 구현한다. streaming state·레지스터 보존·호출 경계와 code memory 정책을 확인한다.
4. 실제 지원 기기에서 기준 ARM64 경로와 동일 출력·end-to-end 비용을 비교한다. macOS 지원은 해당 SDK와 native 실행 검증을 별도로 요구한다. x64와 ARM64의 프런트엔드·GPU·입력·오디오 수락을 서로 대체하지 않는다.
5. 효용이 확인된 CPU/OS/커널 조합만 선택기로 노출한다. 실행 미검증 또는 미지원 후보는 연구/조건부 상태로 두며 완료했다고 표시하지 않는다.

<a id="v42"></a>

## 1.1.42 — Linux·BSD 프런트엔드 수락

기본 과제: BV-14. 공동·후속: BV-03, CJ-16.

소스 시작점: [CMakeLists.txt](../CMakeLists.txt), [context_egl_wayland.cpp](../src/frontend/graphics/gl/context_egl_wayland.cpp), [context_glx.cpp](../src/frontend/graphics/gl/context_glx.cpp), [ARMJIT_Memory.cpp](../src/ARMJIT_Memory.cpp).

기존 회귀 시작점: `core-.*execution|gpu-.*frame-capture|qt-keyboard|audio`.

1. Linux X11/Wayland와 FreeBSD/OpenBSD/NetBSD의 목표 지원 조합·로컬 의존성·JIT 메모리 API를 정한다. 과거 빌드 기록을 현재 소스의 통과로 재사용하지 않는다.
2. 최소 local configure/build/launch와 플랫폼 API 차이를 해결한다. 하나의 BSD 결과를 다른 BSD에 확장하지 않고 기능별 미지원 상태를 명시한다.
3. Qt 창/입력·오디오/마이크·OpenGL surface·파일 경로·network·JIT 옵션을 실제 사용할 조합으로 연결한다. Vulkan은 해당 장치/driver가 준비된 경우 별도 gate를 받는다.
4. 목표 OS에서 실제 프런트엔드 게임 실행·터치/패드·저장/상태 재개·렌더러 재생성·장치 전환·종료를 확인한다. compile-only와 runtime/장치 결과를 구분한다.
5. 각 OS의 설치/실행 자료와 지원 조합을 독립 배포한다. macOS는 별도 플랫폼 범위로 관리한다.

<a id="v43"></a>

## 1.1.43 — 버전·패키지·복구 가능한 배포

기본 과제: BV-02, BV-03, BV-04, BV-05, BV-13. 공동·후속: BV-18, BV-19.

소스 시작점: [CMakeLists.txt](../CMakeLists.txt), [package-windows.py](../tools/package-windows.py), [msys-dist.sh](../tools/msys-dist.sh), [ci-current-build.py](../tools/ci-current-build.py).

기존 회귀 시작점: `cxx26|qt-keyboard|utf8`.

1. 표시 버전과 숫자 tuple, EXE/resource/launcher/ZIP의 현재 생성 경로를 대조한다. 1.1.09→1.1.10→1.1.100과 1.1.0191 표시 정책을 확정한다.
2. 숫자 patch 191과 표시 문자열 0191을 분리하고 숫자 정렬·상태/설정 호환 버전의 역할을 문서화한다. Windows numeric resource 각 16-bit 한계는 별도 관리한다.
3. 로컬 build profile·필요 DLL/plugin closure·분리 debug symbols·license/manifest를 정리한다. 포함 allowlist와 저장 데이터 보존 규칙으로 배포 ZIP을 만든다.
4. 깨끗한 PATH에서 --help와 실제 frontend 실행, 선택 기능의 DLL/plugin 로딩·설정 migration·게임 저장·unzip 재실행을 확인한다. ZIP member/hash와 private asset 부재를 검사한다.
5. 동일 소스에서 실행/소스/검증 자료를 재생성할 수 있어야 배포한다. 정리 대상은 생성된 산출물로 제한하고 사용 중 파일과 저장 데이터를 보존한다.

<a id="v44"></a>

## 1.1.44 — 전체 게임·실기·실험 기능 승격 검증

기본 과제: BV-17, BV-19. 공동·후속: BV-14, BV-15, BV-16.

소스 시작점: [ROMSmoke.cpp](../tests/ROMSmoke.cpp).

기존 회귀 시작점: 선택 구성의 전체 CTest 1회 (`-R` 없이 실행).

1. 각 기능의 기능완료·회귀·장치·장기·실기 조건과 현재 미충족 항목을 모은다. DS 기본 사용과 DSi/Vulkan/Netplay의 승격 기준을 각각 확정한다.
2. 저장/복원·입력/터치·오디오·GPU readback·JIT·네트워크·주변장치가 교차하는 소수의 실제 게임 시나리오를 준비한다. 게임 파일과 저장 데이터를 배포 자료에 포함하지 않는다.
3. 재현된 통합 실패는 원래 담당 ID로 되돌려 최소 수정한다. 옵션 조합은 대표 pairwise와 발견된 위험 조합을 사용하고 모든 조합의 형식적 전수 실행을 피한다.
4. Windows의 장시간 게임·잠금/휴면·장치 상실·저장 재시작과 DS/DSi 및 DSP 실기 비교 결과를 수집한다. ARM/BSD/Apple은 각각 확보된 native gate만 별도 합류한다.
5. 데이터 보존·오류 복구·성능과 실제 기능 수락을 만족한 기능만 안정 상태로 승격한다. 미검증 장치나 실기 oracle 부족은 독립 조건부 항목으로 남기고 다른 안정 버전의 배포를 막지 않는다.
