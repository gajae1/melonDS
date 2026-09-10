# 검증 방법과 기록

설치된 CMake·컴파일러·Qt·SDL 및 의존성을 사용한다. 상세 설치 방법은 [BUILD.md](../BUILD.md), 기존 테스트 구조는 [tests/README.md](../tests/README.md)를 따른다.
같은 개발 빌드 폴더를 재사용한다. 아래 명령은 저장소 루트 기준이며 기기 식별 정보·개인 경로·게임 파일을 요구하지 않는다.

## Windows 개발 구성

의존성 도구의 실행 파일과 DLL을 찾을 수 있는 셸에서 실행한다. AVX-512 옵션은 해당 커널을 빌드하며 실제 사용 여부는 CPU/OS 검출로 결정한다.
GPU 테스트는 실행 가능한 OpenGL context가 필요하다. GCC LTO 예외는 기존 설정을 유지한다.

```sh
cmake -S . -B build/windows-dev -G Ninja -DCMAKE_BUILD_TYPE=Release -DMELONDS_BUILD_TESTS=ON -DMELONDS_TEST_GPU=ON -DENABLE_AVX512=ON -DENABLE_LTO=OFF -DENABLE_LTO_RELEASE=OFF
cmake --build build/windows-dev --parallel 8
ctest --test-dir build/windows-dev --no-tests=error --output-on-failure -R "qt-keyboard-mapping-input|rom-zstd-decompression|core-.*execution|savestate-"
```

새 반례를 추가한 뒤 수정 전 실패와 수정 후 성공을 확인한다. 소스가 바뀌면 먼저 해당 target을 다시 빌드한다.
GPU·JIT·파일 실패 주입의 skip은 성공으로 계산하지 않는다. 새 후보의 검사는 위 selector만으로 자동 확보되지 않는다.
상태 복원·저장 잠금 증분은 `SavestateLoad`, `StateLoadMessages`, `SaveManagerIO`를 빌드한 뒤 `savestate-load-|savestate-rollback-|savestate-message-recovery|save-manager-`로 검사한다. 코어·메시지 정책·실제 저장 worker를 각각 검증하며 이 결과를 물리 오디오 장치나 게임 장기 실행 수락으로 확대하지 않는다.
ROM 교체·종료 증분은 `CartReplacement`, `FrontendClose`, `SaveManagerIO`와 `cart-replacement-|frontend-close-|save-manager-`를 사용한다. FrontendClose는 Qt의 offscreen 환경에서 자체 생성한 창·대화상자를 실행하며 사용자 데스크톱이나 개인 파일을 조작하지 않는다.
파일 준비·가져오기 증분은 `FrontendFileIO`, `ArchiveIO`, `StateLoadMessages`, `CartReplacement`와 `frontend-file-|archive-io-|save-import-|cart-replacement-ds-import-partial`을 사용한다. 실제 생성 archive·카트 SRAM·RTC와 파일 오류 주입의 범위를 구분한다.
입력·마이크 증분은 `FrontendTouch`, `TouchPublication`, `FrontendJoystick`, `FrontendMicrophone`과 `frontend-touch-|frontend-joystick-|frontend-microphone-`를 사용한다. Qt offscreen·SDL 가상 장치·보호 페이지·생산/소비 대조이며 물리 입력/마이크 장치는 사용하지 않는다. 가상 장치 검사의 skip은 실제 실행 성공으로 계산하지 않는다.
네트워크 증분은 `LANPacket`, `PCapInput`, `SlirpDNS`, `GdbProtocol`과 `lan-packet-|pcap-input-|slirp-dns-|gdb-protocol-`를 사용한다. LAN은 실제 ENet packet/queue에 생성 이벤트를 넣고, PCap은 library/driver 경계와 보호 페이지를 사용한다. DNS는 현재 production 함수 정의를 빌드 때 추출하며 resolver/slirp를 대체한다. GDB는 실제 production stub에 transport 오류를 주입하고 `loopback`에서는 실제 TCP의 분할 수신·escape·NoAck 재접속·detach를 검사한다. loopback listener는 테스트에서 로컬 주소로 한정한다. 실제 NIC·DNS·상용 게임·ARM GDB/LLDB client 수락과 구분한다.
홈브루·치트 증분은 `HomebrewInput`, `ARDatabaseInput`, `ARExecution`, `CheatImportUI`, `CheatCancellation`과 `homebrew-|ar-database-|ar-execution-|cheat-import-|cheat-message-`를 사용한다. DLDI/argv는 실제 함수에 SD·메모리 경계를 대체하고, DB는 실제 parser와 Qt 임시 파일에 read/seek 실패를 주입한다. 치트 실행은 실제 ARM7 VBlank hook과 코어 메모리를 사용한다. 메시지 검사는 실제 Qt 게시·토큰 전달·대기와 제어된 worker의 큐 확인까지이며 전체 창·장치·게임 수락을 뜻하지 않는다.
최종 구성의 전체 검사는 필요할 때 한 번 실행하고 범위가 같은 성공 결과를 재사용한다.

저장 정체성·주변장치 증분은 `AssetIdentityTest`, `AssetIdentityUI`, `CartReplacement`, `IRCartState`, `RTCCalendar`, `GBARumble`과 `asset-identity-|asset-ui-|cart-replacement-.*asset|ir-cart-|rtc-|gba-rumble-`를 사용한다. registry와 게임 파일은 서로 다른 임시 폴더이며 Windows alias case는 임시 경로 안의 junction만 생성한다. IR/진동은 실제 카트 코드, RTC는 전체 생산 RTC와 현재 scheduler 정의를 실행한다. 합성 IRQ·진동 callback과 offscreen 메시지 경계를 물리 장치나 전체 GUI 수락으로 확대하지 않는다.

카트 SPI·EEPROM 증분은 `CartSPI`, `RetailEEPROM`과 `cart-spi-|retail-eeprom-`를 사용한다. CartSPI는 실제 NDS core의 ARM9/ARM7 MMIO·scheduler·retail cart·전체 savestate를 사용하며 SPI callback 관찰 뒤 생산 메서드에 그대로 위임한다. CPU 명령 실행/JIT 검사는 별도다. RetailEEPROM은 실제 SPI 입력·SRAM·상태에 생성 저장 image를 연결해 통지 범위만 재생한다. 메모리 image 일치를 실제 디스크 commit이나 실물 chip timing의 증거로 확대하지 않는다.

```sh
ctest --test-dir build/windows-dev --no-tests=error --output-on-failure
```

치트 저장은 `ARCodeFileIO`, `CheatSaveUI`와 `ar-code-file-|cheat-save-ui-`를 사용한다. 실제 mch parser·serializer와 Qt 파일 연산, 실제 편집 창·버튼·메시지 상자를 실행한다. 저장 완료 slot과 실행 목록 갱신 메서드는 현재 소스에서 추출하고 에뮬레이션의 pause/message acknowledgement 경계는 대역으로 제공한다. 생성 파일의 부분 쓰기·commit 실패 보존은 물리 전원 손실이나 실제 게임 메모리 적용의 수락 결과가 아니다. legacy 코어 Save는 쓰기 결과를 반환하며 원자적 파일 교체는 Qt 편집 경로의 QSaveFile이 담당한다.

치트 실행은 `ARExecution`과 `ar-execution-`에서 생산 ARM7 VBlank IRQ hook을 사용한다. C0 교체·D1/D2 조건 복원·D0 기본 상태·data/offset 유지·literal/loop/copy 취소를 생성 guest 메모리로 대조한다. C5 mask 검사는 수명과 무관한 참/거짓 입력만 사용하며 지역 counter의 현재 결과를 Datel 수명의 정답으로 등록하지 않는다.

입력 설정은 `InputConfigUI`와 `input-dialog-`에서 실제 Qt dialog·매핑 버튼·Config 파일 연산을 사용한다. 가상 SDL 컨트롤러 연결 여부, 명시적 탭 전환, 키 지정·취소·해제, OK 적용·이전 키 비활성화·새 프로세스 재로드를 검사한다. `FrontendInput`은 modifier·focus와 손상 설정 보존 검사를 유지한다. Qt 내부에 전달한 이벤트와 물리 장치·OS IME·게임 입력 지연은 별도 수락이다.

ALU·shift는 `CoreExecution`과 `core-.*-(alu-shift|thumb-shift-timing)`을 사용한다. 실제 ARM9/ARM7 코어·scheduler에 생성 guest 명령을 실행하고 두 번째 진입으로 JIT 컴파일 때의 interpreter 실행과 구분한다. 경계값·carry/overflow·alias·부분 flags·RRX·PC 피연산자를 수기 기대값/확장 정수 연산과 대조한다. Thumb LSR/ASR의 같은 양수 입력 루프는 한 프레임의 반복 횟수로 내부 사이클 누락을 검출한다. 이 상대 비교는 DS 실기의 절대 타이밍 수락이 아니다. 게임 digest가 interpreter/JIT 사이에서 다르면 같은 입력의 이전 코어도 비교하고, 기존 차이를 새 수정의 회귀나 완전한 동등성으로 단정하지 않는다.

목록 전송은 `CoreExecution`의 `core-.*-block-transfer`에서 ARM9/ARM7 실제 guest 프로그램으로 검사한다. IA/IB/DA/DB·W=0/1·조건 결과가 바뀌는 warmed 진입, 정상 목록 대조·PC 저장값·Thumb 유지·SPSR/은행 SP 복원·같은 pipeline PC에서의 상태 변경을 포함한다. ARM9 비전송은 실제 CPU의 data bus 호출을 계수하고 이전 DataCycles를 바꿔 비용 독립성을 확인한다. CPU별 호환 모델의 회귀와 직접 DS 실리콘 측정은 구분한다. 빈 PUSH/POP은 원 oracle을 확보하기 전 기대값을 만들어 등록하지 않는다.

DTCM은 `CoreExecution`의 `core-.*-dtcm-remap`에서 CP15 이동·해제와 실제 guest 재진입을 검사한다. Windows fastmem은 자신의 OS view를 조회해 예약/매핑·값·코드 보호를 독립 확인한다. 값만 일치하는 느린 helper 전환과 구분하며 다른 플랫폼의 실제 view 검증으로 확대하지 않는다. `core-scheduler-execution`은 실제 NDS의 ScheduleEvent/CancelEvent/RunSystem을 사용해 취소·재예약·교체·periodic·snapshot·정상 순서를 검사한다. 이 생성 callback 계약과 IRQ/DMA/sleep 장치 전체 타이밍은 별도다.

## 진단 구성

`gpu-gl-allocation-`은 실제 할당 오류와 모의 한도/OOM을 분리하고 캡처 보존·실제 software renderer·오류 통지·재선택을 확인한다. 실제 물리 OOM은 GL 상태를 보장하지 않으므로 core fallback을 전체 표시 context 복구로 보고하지 않는다. `gl-borrow-`는 현재 생산 메시지 case/완료 알림/반환과 실제 Qt 대기를 사용해 두 순서를 결정적으로 검사한다. GL release는 대역이며 실제 panel 파괴·GLAD 로딩·전체 GUI 수명은 후속 gate다.

`gl-presentation-runtime-current|runtime-swap|retire-current`는 초기화 이후 실패 반환·재시도와 root 해제 거부를 검사한다. `gl-state-message-gate`는 실패 중 core 소비 차단·오디오 중단·오류 통지·복구를 검사한다. [1.1.24](releases/1.1.24.md)의 전체 Qt 생성 DS 검증은 실제 창·worker에 WGL 오류만 주입한 별도 근거이며 물리 surface 상실·실기 입력·상용 게임 수락과 구분한다.

`gl-loader-release-`는 worker 반환 실패의 GUI 진입 거부·부분/중첩 소유권과 새 context 반환 실패를 검사한다. `frontend-close-app-state`는 닫힌 창에 앱 상태 알림을 전달한다. [1.1.25](releases/1.1.25.md)의 전체 Qt 세 인스턴스·네 창 검증에서 WGL 실패 wrapper는 current를 일부러 유지하므로 문서상 실제 WGL 오류·물리 장치 상실의 재현으로 집계하지 않는다.

[1.1.26](releases/1.1.26.md)은 `video-settings-dialog-recovery`, `direct-boot-message-failure`, `dsi-boot26-`, `dsi-sd-`, `localmp-`를 추가했다. 부팅은 독립 암호 vector와 실제 core/FatFs의 합성 NAND, SD는 현재 controller와 실제 Qt 파일 I/O, LocalMP는 현재 구현과 결정적인 수신 순서를 사용한다. 기존 core·파일·Qt 시험 도구를 재사용하고 fixture 결과를 실제 게임·장치 수락으로 확대하지 않는다. 전체 Qt 조작은 modal 안내를 먼저 처리한 뒤 설정 변경·종료를 진행한다.

[1.1.27](releases/1.1.27.md)의 `software-thread-`는 실제 rasterizer의 semaphore 경계와 전체 core 상태 왕복을, `core-dsi-ndma`는 실제 MMIO·GX FIFO 포화·명령/IRQ·snapshot을 검사한다. `wifi-state-`는 실제 SDIO CMD52·HTC/WMI event·IRQ와 생성 packet을 사용한다. worker의 host 순서, 수동 device callback, 짧은 DMA 관찰 slice를 guest 실행·물리 timing·장기 게임 결과로 집계하지 않는다.

[1.1.28](releases/1.1.28.md)의 `gpu-*-capture-mid`는 실제 ARM9 프로그램·frame scheduler·DMA에서 캡처 도중의 읽기·쓰기를 확인한다. `dsi-aes-`는 공개 암호 벡터와 실제 MMIO·NDMA·전체 상태 복원을, `core-dsi-reset-i2c`는 controller/slave·IRQ·reset 경로와 구형 상태 호환을 확인한다. AES/I2C의 host 구동 장치 시험과 캡처의 실제 guest CPU 실행을 구별하며 물리 latency·게임 수락을 대신하지 않는다.

`gpu-.*-capture-readback`은 실제 source A 3D/B·혼합 캡처, 1x/2x, pending/CPU-synced 배율 전환과 guest ARM9 LDRH·후속 texture 재사용을 검사한다. 주색 끝값·정수 합으로 기존 중간 강도 양자화 차이를 분리하며 전체 색 정밀도 oracle로 사용하지 않는다. `gpu-gl-resource-`는 실제 생성/삭제에 위임하여 같은 context에서 정상 반복·첫 shader 오류 뒤 program/buffer/texture 생존과 대조 객체 보존을 확인한다. 미초기화 읽기의 원래 재현은 compiler별로 달라질 수 있고 이 검사는 context 상실·전체 Qt event loop 수락이 아니다.

`core-.*-device-execution`은 실제 Timer0 MMIO·HALT·IRQ의 F 보존과 IME/IE/CPSR 마스크 다섯 조건을 생성 ARM9 guest로 검사한다. handler 시각은 최초 overflow 이전이 아님을 확인하며 절대 실기 latency를 추정하지 않는다.

`gpu-compute-frame-capture`는 source B 캡처를 실제 texture로 사용하는 S/T sampler variant의 binding·픽셀까지 검사한다. `gpu-compute-failure-`는 실제 driver compile/link 실패, 주입한 capability 판정, 배율 재컴파일과 현재 프런트엔드 메서드의 software 복구·재선택을 검사한다. dispatch 시도 계수와 실제 픽셀 렌더링은 별도 case다. 설정 읽기·OSD를 격리한 메서드 실행을 Qt 전체 event loop로, GL4.3 조회 주입을 3.2-only 장치로, source 재컴파일을 비활성 binary cache-hit으로 확대하지 않는다.

MPU 실행 권한은 `CoreExecution`의 `core-.*-mpu-execution`에서 실제 guest MCR·정적/동적 분기·warmed block을 사용한다. 정상/금지/재허용, exception 상태와 register/RAM 부작용을 대조한다. 모든 data abort와 실제 silicon timing의 수락으로 확대하지 않는다.

오디오 공급은 `FrontendAudio`의 `audio-callback-buffer`와 `AudioResampling`의 `audio-core-clock`으로 생성 PCM 부족/복귀와 코어 FIFO 덮어쓰기를 확인한다. 실제 장치 측정은 [ROMSmoke의 선택 환경변수](../tests/README.md)를 사용해 같은 구간의 보간·버퍼를 대조한다. 출력 제출은 무음이며 GUI presentation·장기 청취·물리 지연 검사를 대신하지 않는다. 일반 실행의 `MELONDS_AUDIO_DIAGNOSTICS=1`은 pause/종료 로그를 켜고 기본 비활성 상태에서는 callback 시간을 측정하지 않는다.

루트 [Sanitizers.cmake](../cmake/Sanitizers.cmake)의 `SANITIZE` 설정은 도구와 runtime이 지원하는 조합에서 사용한다. 작은 parser·buffer 경계는 기존 독립 테스트를 우선한다.
JIT fastmem의 의도된 fault와 실제 메모리 오류를 구분한다. sanitizer runtime이 없거나 충돌하면 실행하지 못한 이유를 남기며 전체 검사를 꺼서 통과로 바꾸지 않는다.
ASan/UBSan/TSan의 지원 조합 표와 추가 JIT 통합은 BV-11의 후속 과제다. 이 문서 자체는 sanitizer 실행 증거가 아니다.

## 증거와 측정

[1.1.29](releases/1.1.29.md)의 `lan-packet-validation`, `lan-loopback-session`, `lan-ui-session`은 packet 경계와 실제 loopback/Qt 연결·취소·재시도를 구분한다. `input-dialog-selection-`과 `frontend-joystick-`는 가상 장치·임시 설정·일부 serial/센서 대역을 사용한다. `dsi-title-`는 생성 NAND·실제 FatFs/암호/I/O와 통제한 실패 경계를 사용한다. 실제 매체·게임·실물 입력·다른 OS 결과로 확대하지 않는다. 같은 최종 소스의 전체 548/548 성공은 재사용한다.

[1.1.30](releases/1.1.30.md)의 같은 LAN fixture는 실제 4명·늦은 참가/준비·클라이언트 command/reply·ID 재사용을 추가한다. `savestate-load-audio-`는 성공/실패 복귀 뒤 실제 SDL dummy callback과 새로 생성한 PCM을 대조한다. `gpu-*-capture-jit`는 source A의 DMA 첫 소비 뒤 동일 JIT 블록을 재사용하며, 캡처를 끈 poison 대조도 확인한다. native VRAM fastmem mapping·JIT-first·실기/물리 장치 검증으로 확대하지 않는다.

한 변경 기록에 소스 기준·변경 ID·빌드 종류·compiler/의존성 버전·관련 옵션·실행 명령·입력 종류·기대 결과·실패/성공/skip·남은 범위를 적는다.
재현 입력은 합성 자료를 우선하고 실제 게임·저장 파일은 공개 자료에 넣지 않는다. 공개 가능한 digest와 필요한 비식별 환경 정보만 사용한다.

성능 결과는 같은 소스 조건·입력·출력·renderer·JIT 설정의 전후 비교로 기록한다. warm/cold, 중앙값과 분산, CPU/GPU/upload/readback/audio 대기를 구분한다.
실측하지 않은 percentile이나 성능 이득을 채우지 않는다. 커널 시간 단축을 전체 FPS·입력 지연 개선으로 환산하지 않는다.
compiler profile을 학습한 workload와 평가 workload를 분리한다. 소스·compiler가 바뀐 PGO profile은 재검증한다.

패키지는 생성된 실행 파일·필요 DLL/plugin·라이선스만 선별한다. 원본 저장 데이터는 보존한다.
독립 PATH에서 실행 파일의 `--help`와 필요한 frontend·plugin 경로를 확인한다. `--help` 성공은 실제 게임·장치 수락을 대신하지 않는다.

## 구버전 이관·통신과 보안 후속

[1.1.31](releases/1.1.31.md)의 `fat-storage-`는 실제 생성 FAT 이미지와 현재 Qt I/O/FatFs에서 마운트 읽기·탐색·길이 오류, 비정상 파일과 새 포맷 실패를 주입한다. 이미지·인덱스·폴더의 바이트 보존, handle 닫기, 같은 이미지 재시도를 확인하며 `cart-replacement-invalid-sd`는 실제 DSi/카트 소유자가 실패 객체를 삽입하지 않는지 검사한다. `core-dsi-btdmp`는 한-word checked STL 반례와 실제 I2S/DSP 호출을 확인한다. 부족 채널 0은 명시한 안전정책이며 실기 oracle이 아니다. Compute 초기화는 기존 frame/capture 세 실행의 전후 출력이 같았고 성능 향상으로 세지 않는다.

호환성 변경은 같은 버전끼리의 성공만으로 완료하지 않는다. 원본/구버전 writer가 만든 일반 게임 저장·설정·중간 저장을 새 reader에 전달하고, LAN은 호스트 역할을 바꾼 연결·게임 패킷 왕복을 확인한다. 포맷 major가 다른 중간 저장을 헤더 수정만으로 허용하지 않으며, 업그레이드와 구버전으로 되돌리기의 지원 방향을 구분한다. [1.1.30 기록](releases/1.1.30.md)은 원본 소스의 설정 이관 성공 및 LAN 클라이언트 방향의 미통과를 함께 남긴다.

후속 보안 점검은 기존 NP-01/02·CJ-10·FS-01/02에 연결한다. 패킷/파일 길이·peer 권한·처리량과 큐의 상한·불량 입력 뒤 복구를 생성 입력으로 확인한다. 통신 지연을 줄이기 위해 검증을 제거하지 않으며, 정상 입력의 처리 비용과 오류 입력의 자원 사용을 별도로 측정한다. 이 문단은 점검 계획이며 보안 감사 완료나 침투 시험 성공을 뜻하지 않는다.
