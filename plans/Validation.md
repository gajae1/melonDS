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
파일 준비·가져오기 증분은 `FrontendFileIO`, `ArchiveIO`, `StateLoadMessages`, `CartReplacement`와 `frontend-file-|archive-io-|save-import-|cart-replacement-ds-import-partial`을 사용한다. `ROMPreparationUI`의 `rom-preparation-`는 생산 준비 코드·창 메서드에 생성 지연 IO와 Qt 이벤트를 연결해 취소·재선택·종료를 확인한다. `cart-replacement-.*prepared`와 `asset-ui-prepared-modal-cancel`은 실제 카트/저장 및 저장 파일 충돌 선택 중 취소를 확인한다. 실제 생성 archive·카트 SRAM·RTC와 파일 오류 주입의 범위를 구분하며 물리 매체의 응답 시간이나 전체 GUI 수락으로 확대하지 않는다.
입력·마이크 증분은 `FrontendTouch`, `TouchPublication`, `FrontendJoystick`, `FrontendMicrophone`과 `frontend-touch-|frontend-joystick-|frontend-microphone-`를 사용한다. Qt offscreen·SDL 가상 장치·보호 페이지·생산/소비 대조이며 물리 입력/마이크 장치는 사용하지 않는다. 가상 장치 검사의 skip은 실제 실행 성공으로 계산하지 않는다.
네트워크 증분은 `LANPacket`, `PCapInput`, `SlirpDNS`, `GdbProtocol`과 `lan-packet-|pcap-input-|slirp-dns-|gdb-protocol-`를 사용한다. LAN은 실제 ENet packet/queue에 생성 이벤트를 넣고, PCap은 library/driver 경계와 보호 페이지를 사용한다. DNS는 현재 production 함수 정의를 빌드 때 추출하며 resolver/slirp를 대체한다. GDB는 실제 production stub에 transport 오류를 주입하고 `loopback`에서는 실제 TCP의 분할 수신·escape·NoAck 재접속·detach를 검사한다. loopback listener는 테스트에서 로컬 주소로 한정한다. 실제 NIC·DNS·상용 게임·ARM GDB/LLDB client 수락과 구분한다.
홈브루·치트 증분은 `HomebrewInput`, `ARDatabaseInput`, `ARExecution`, `CheatImportUI`, `CheatCancellation`과 `homebrew-|ar-database-|ar-execution-|cheat-import-|cheat-message-`를 사용한다. DLDI/argv는 실제 함수에 SD·메모리 경계를 대체하고, DB는 실제 parser와 Qt 임시 파일에 read/seek 실패를 주입한다. 치트 실행은 실제 ARM7 VBlank hook과 코어 메모리를 사용한다. 메시지 검사는 실제 Qt 게시·토큰 전달·대기와 제어된 worker의 큐 확인까지이며 전체 창·장치·게임 수락을 뜻하지 않는다.
최종 구성의 전체 검사는 필요할 때 한 번 실행하고 범위가 같은 성공 결과를 재사용한다.

저장 정체성·주변장치 증분은 `AssetIdentityTest`, `AssetIdentityUI`, `CartReplacement`, `IRCartState`, `RTCCalendar`, `GBARumble`과 `asset-identity-|asset-ui-|cart-replacement-.*asset|ir-cart-|rtc-|gba-rumble-`를 사용한다. registry와 게임 파일은 서로 다른 임시 폴더이며 Windows alias case는 임시 경로 안의 junction만 생성한다. IR/진동은 실제 카트 코드, RTC는 전체 생산 RTC와 현재 scheduler 정의를 실행한다. 합성 IRQ·진동 callback과 offscreen 메시지 경계를 물리 장치나 전체 GUI 수락으로 확대하지 않는다.

카트 SPI·EEPROM 증분은 `CartSPI`, `RetailEEPROM`과 `cart-spi-|retail-eeprom-`를 사용한다. CartSPI는 실제 NDS core의 ARM9/ARM7 MMIO·scheduler·retail cart·전체 savestate를 사용하며 SPI callback 관찰 뒤 생산 메서드에 그대로 위임한다. `cart-spi-key1-state`는 생성 BIOS/ROM의 암호화 명령을 새 console·cart에 복원해 두 CPU의 chip ID를 확인하고 DSi 타입의 상태 API로 NWRAM을 복원한다. CPU 명령 실행/JIT 검사는 별도다. RetailEEPROM은 실제 SPI 입력·SRAM·상태에 생성 저장 image를 연결해 통지 범위만 재생한다. 메모리 image 일치를 실제 디스크 commit이나 실물 chip timing의 증거로 확대하지 않는다.

GBA 초기 저장 판별은 `GBASave`의 `gba-save-detection`으로 확인한다. 생성 ROM의 완전한 SDK 마커 다섯 종류, 첫 쓰기 프로토콜·저장 통지, 기존20개 저장 입력의 길이/바이트 보존과 불완전·상충·EEPROM 마커를 검사한다. 1.1.69는 수정 전 다섯 초기 용량 실패를 재현한 뒤 통과했고 JIT OFF의 GBA 저장8검사도 통과했다. 별도 실제 Qt 메시지·Slot-2·SaveManager 실행으로 저장 파일이 없는 삽입→첫 쓰기→파일 commit→재삽입, 기존 EEPROM 파일 우선과 새 저장 할당 실패의 기존 카트/manager 보존을 확인했다. 이 SDK fallback을 전체 ROM DB·실물 칩 식별이나 미등록 EEPROM 용량 판별 완료로 세지 않는다.

1.1.70의 `gba-save-database`는 MAME 원본의512B/8KiB 참조 행과 길이 불일치를 검사한다. `gba-save-database-generator`는 ROM 없이 합성 XML의 고정 용량·전체 해시·중복·충돌·불완전 dump와 잘못된 pin/입력의 기존 출력 보존을 검증한다. 원본 XML은 공개 출처에서 별도 확보하고 `python tools/generate-gba-save-db.py path/to/gba.xml src/GBASaveDatabase.h --check`로 재현한다. 일반 빌드·CTest는 네트워크나 원본 XML을 요구하지 않는다. 카탈로그 변환/lookup 검증과 생성 식별표를 이용한 실제 factory·칩 실행, 실제 게임 ROM/실물 검증을 구분한다.

수동 초기 GBA 저장 선택은 `CartReplacement`의 실제 로더·카트·SaveManager 경로와 `AssetIdentityUI`/`ROMPreparationUI`의 Qt 선택·취소·메시지 전달을 구분해 검증한다. 기존 파일 우선, 없는 파일의 첫 게임 쓰기, 빈 기존 파일·잘못된 용량의 거부와 현재 카트 보존이 필수다. UI의 메시지 기록만으로 실제 저장 성공을 주장하지 않으며, 별도의 실제 앱 객체·에뮬레이션 스레드·선택창 실행으로 두 경로를 연결한다.

```sh
ctest --test-dir build/windows-dev --no-tests=error --output-on-failure
```

치트 저장은 `ARCodeFileIO`, `CheatSaveUI`와 `ar-code-file-|cheat-save-ui-`를 사용한다. 실제 mch parser·serializer와 Qt 파일 연산, 실제 편집 창·버튼·메시지 상자를 실행한다. 저장 완료 slot과 실행 목록 갱신 메서드는 현재 소스에서 추출하고 에뮬레이션의 pause/message acknowledgement 경계는 대역으로 제공한다. 생성 파일의 부분 쓰기·commit 실패 보존은 물리 전원 손실이나 실제 게임 메모리 적용의 수락 결과가 아니다. legacy 코어 Save는 쓰기 결과를 반환하며 원자적 파일 교체는 Qt 편집 경로의 QSaveFile이 담당한다.

치트 실행은 `ARExecution`과 `ar-execution-`에서 생산 ARM7 VBlank IRQ hook을 사용한다. C0 교체·D1/D2 조건 복원·D0 기본 상태·data/offset 유지·literal/loop/copy 취소를 생성 guest 메모리로 대조한다. C5 mask 검사는 수명과 무관한 참/거짓 입력만 사용하며 지역 counter의 현재 결과를 Datel 수명의 정답으로 등록하지 않는다.

입력 설정은 `InputConfigUI`와 `input-dialog-`에서 실제 Qt dialog·매핑 버튼·Config 파일 연산을 사용한다. 가상 SDL 컨트롤러 연결 여부, 명시적 탭 전환, 키 지정·취소·해제, OK 적용·이전 키 비활성화·새 프로세스 재로드를 검사한다. `FrontendInput`은 modifier·focus와 손상 설정 보존 검사를 유지한다. Qt 내부에 전달한 이벤트와 물리 장치·OS IME·게임 입력 지연은 별도 수락이다.

ALU·shift는 `CoreExecution`과 `core-.*-(alu-shift|thumb-shift-timing)`을 사용한다. 실제 ARM9/ARM7 코어·scheduler에 생성 guest 명령을 실행하고 두 번째 진입으로 JIT 컴파일 때의 interpreter 실행과 구분한다. 경계값·carry/overflow·alias·부분 flags·RRX·PC 피연산자를 수기 기대값/확장 정수 연산과 대조한다. Thumb LSR/ASR의 같은 양수 입력 루프는 한 프레임의 반복 횟수로 내부 사이클 누락을 검출한다. 이 상대 비교는 DS 실기의 절대 타이밍 수락이 아니다. 게임 digest가 interpreter/JIT 사이에서 다르면 같은 입력의 이전 코어도 비교하고, 기존 차이를 새 수정의 회귀나 완전한 동등성으로 단정하지 않는다.

목록 전송은 `CoreExecution`의 `core-.*-block-transfer`에서 ARM9/ARM7 실제 guest 프로그램으로 검사한다. IA/IB/DA/DB·W=0/1·조건 결과가 바뀌는 warmed 진입, 정상 목록 대조·PC 저장값·Thumb 유지·SPSR/은행 SP 복원·같은 pipeline PC에서의 상태 변경을 포함한다. ARM9 비전송은 실제 CPU의 data bus 호출을 계수하고 이전 DataCycles를 바꿔 비용 독립성을 확인한다. CPU별 호환 모델의 회귀와 직접 DS 실리콘 측정은 구분한다. 빈 PUSH/POP은 원 oracle을 확보하기 전 기대값을 만들어 등록하지 않는다.

DTCM은 `CoreExecution`의 `core-.*-dtcm-remap`에서 CP15 이동·해제와 실제 guest 재진입을 검사한다. Windows fastmem은 자신의 OS view를 조회해 예약/매핑·값·코드 보호를 독립 확인한다. 값만 일치하는 느린 helper 전환과 구분하며 다른 플랫폼의 실제 view 검증으로 확대하지 않는다. `core-scheduler-execution`은 실제 NDS의 ScheduleEvent/CancelEvent/RunSystem을 사용해 취소·재예약·교체·periodic·snapshot·정상 순서를 검사한다. 이 생성 callback 계약과 IRQ/DMA/sleep 장치 전체 타이밍은 별도다.

SMC는 기존 `core-.*-smc-reentry`와 `CoreSMCRemapExecution`의 `core-.*-smc-(dma|remap-.*)`를 사용한다. 생성 명령·실제 DMA/MMIO로 코드 덮어쓰기, SWRAM/NWRAM A/B/C 재매핑 후 ARM9/7·ARM/Thumb 재진입, 이전 backing에 대한 후속 DMA, alias·이웃 코드 보존을 확인한다. JIT의 두 번째 실행은 추가 코드 생성이 없는지 검사한다. `ENABLE_JIT=OFF`에서도 같은 interpreter 회귀를 실행하며 미지원 fastmem은 성공으로 세지 않는다. 활성 블록 중 remap·겹친 NWRAM broadcast·교차 영역 literal·native A64와 실기 절대 cycle은 별도 수락 범위다.

## 진단 구성

GBA EEPROM은 `gba-eeprom-chip`, `gba-eeprom-cart-*`, `core-gba-eeprom-*`로 칩 패킷·저장 통지/소유권·실제 ARM9/7 DMA를 각각 검사한다. 512B/8KiB·busy·yield/선점·32비트 beat 순서·전송 중14.5 복원과 reset/import/교체를 포함하며 JIT OFF에서도 실행한다. 초기 미지 용량, 칩 변종 및 CPU/폭/EXMEM 조합의 물리적 성공 범위는 생성 테스트의 수락으로 확대하지 않는다.

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

`gba-save-`는 실제 GBA Flash 명령·저장 callback을64/128KiB와 RTC trailer에서 검사한다. 전체 지우기·기본/태양광 카트의 불완전/잘못된 상태 거부·완전한 상태를 관찰하는 저장 callback·생성13/current 복원과 용량 변경/자기 참조 가져오기를 포함한다. 여분의 소유 backing은 기존 범위 초과를 canary 불일치로 관찰하기 위한 것이며 device 크기에 포함하지 않는다. `core-gba-flash-bus`는 정확한 크기의 버퍼와 실제 ARM9/7 메모리 핸들러·슬롯 소유권·slot 복원/잘린 header를 별도로 확인한다. CPU 명령 실행·EEPROM ROM bus/DMA·모든 Flash 명령·실기 timing·전체 콘솔의 복원 실패 원자성·일반 가져오기/상태 수락으로 확대하지 않는다.

오디오 공급은 `FrontendAudio`의 `audio-callback-buffer`와 `AudioResampling`의 `audio-core-clock`으로 생성 PCM 부족/복귀와 코어 FIFO 덮어쓰기를 확인한다. 실제 장치 측정은 [ROMSmoke의 선택 환경변수](../tests/README.md)를 사용해 같은 구간의 보간·버퍼를 대조한다. 출력 제출은 무음이며 GUI presentation·장기 청취·물리 지연 검사를 대신하지 않는다. 일반 실행의 `MELONDS_AUDIO_DIAGNOSTICS=1`은 pause/종료 로그를 켜고 기본 비활성 상태에서는 callback 시간을 측정하지 않는다.

`audio-one-shot-state`는 DS/DSi의 HOLD 중 설정 변경 전후·전체 저장/복원 후 실제 capture와 Busy를 확인한다. 일반14.2와 조건부14.4 저장을 구분하며,14.4에서 필수인 빈 NC13 레코드의 누락·잘못된 mode와14.3에서의 빈 레코드를 거부한다. 과거14.2 수동중지8상태는 별도 고정 입력으로 가져와 잔여 소리가 재생되지 않는지 대조한다. 14.4는1.1.62 이하에서 읽을 수 없고, 구형 수동중지의 모든 모호성을 복구했다는 뜻은 아니다.

`core-dsi-hle-savestate`는 실제 G711 v0x10 serializer로 만든 DSP seed를 생산 factory에 로드하고 MMIO 명령을 시작한다. 실제 전체 상태의 callback 재생성과 범위 밖/미등록 FuncID 거부 뒤 정상 재시도, RunFrame의 PCM·응답·IRQ를 대조한다. seed의 core-ID 필드 교체를 공개하며 DSP 프로그램 CRC 인식/부팅·다른 HLE core·codec 정확도·전체 console rollback이나 실기 timing으로 확대하지 않는다.

FS-08의1.1.64 비공개 대용량 검증은 생산 decoder와 실제1GiB 상한을 유지해 정상/초과/잘린·손상 frame10조건의 길이·전체 바이트·거부 시 입력 보존을 확인했다. 작은 공개 회귀와 별도 증거이며 실제 게임 부팅·UI 취소 시각 수락은 아니다. FS-06의 파일 교체 거부는 단계별 로그와 실패 후 읽기 전용 관측만 확보했다. 실패 뒤 점유자0건은 실패 순간의 일시 점유가 없었다는 증거가 아니며, 성공 결과만 남기기 위한 반복 실행·assertion 완화는 하지 않는다.

루트 [Sanitizers.cmake](../cmake/Sanitizers.cmake)의 `SANITIZE` 설정은 도구와 runtime이 지원하는 조합에서 사용한다. 작은 parser·buffer 경계는 기존 독립 테스트를 우선한다.
JIT fastmem의 의도된 fault와 실제 메모리 오류를 구분한다. sanitizer runtime이 없거나 충돌하면 실행하지 못한 이유를 남기며 전체 검사를 꺼서 통과로 바꾸지 않는다.
ASan/UBSan/TSan의 지원 조합 표와 추가 JIT 통합은 BV-11의 후속 과제다. 이 문서 자체는 sanitizer 실행 증거가 아니다.

## 증거와 측정

[1.1.29](releases/1.1.29.md)의 `lan-packet-validation`, `lan-loopback-session`, `lan-ui-session`은 packet 경계와 실제 loopback/Qt 연결·취소·재시도를 구분한다. `input-dialog-selection-`과 `frontend-joystick-`는 가상 장치·임시 설정·일부 serial/센서 대역을 사용한다. `dsi-title-`는 생성 NAND·실제 FatFs/암호/I/O와 통제한 실패 경계를 사용한다. 실제 매체·게임·실물 입력·다른 OS 결과로 확대하지 않는다. 같은 최종 소스의 전체 548/548 성공은 재사용한다.

[1.1.30](releases/1.1.30.md)의 같은 LAN fixture는 실제 4명·늦은 참가/준비·클라이언트 command/reply·ID 재사용을 추가한다. `savestate-load-audio-`는 성공/실패 복귀 뒤 실제 SDL dummy callback과 새로 생성한 PCM을 대조한다. 1.1.62의 `gpu-*-capture-jit`는 RAM에서 준비한 동일 JIT 읽기 블록으로 source A를 CPU-first/DMA-first 순서로 소비한다. 일반 JIT/fastmem, 16/32비트, 128/256폭·bank wrap, GL1x/2x에서 과거/미래 줄과 poison 대조를 확인한다. VRAM은 기존 동기화 fallback을 사용하며 직접 host 매핑·subscanline latch·실기/물리 장치 검증으로 확대하지 않는다.

한 변경 기록에 소스 기준·변경 ID·빌드 종류·compiler/의존성 버전·관련 옵션·실행 명령·입력 종류·기대 결과·실패/성공/skip·남은 범위를 적는다.
재현 입력은 합성 자료를 우선하고 실제 게임·저장 파일은 공개 자료에 넣지 않는다. 공개 가능한 digest와 필요한 비식별 환경 정보만 사용한다.

성능 결과는 같은 소스 조건·입력·출력·renderer·JIT 설정의 전후 비교로 기록한다. warm/cold, 중앙값과 분산, CPU/GPU/upload/readback/audio 대기를 구분한다.
실측하지 않은 percentile이나 성능 이득을 채우지 않는다. 커널 시간 단축을 전체 FPS·입력 지연 개선으로 환산하지 않는다.
compiler profile을 학습한 workload와 평가 workload를 분리한다. 소스·compiler가 바뀐 PGO profile은 재검증한다.

패키지는 생성된 실행 파일·필요 DLL/plugin·라이선스만 선별한다. 원본 저장 데이터는 보존한다.
독립 PATH에서 실행 파일의 `--help`와 필요한 frontend·plugin 경로를 확인한다. `--help` 성공은 실제 게임·장치 수락을 대신하지 않는다.

DS 저장 보호는 기존 `RetailEEPROM`의 `status-register/write-protection/status-state`, `CartSPI`의 `status-protection`, `CartReplacement`의 `ds-capacity-protection`을 사용한다. WREN 없는 상태 변경·보호 경계·FRAM 주소 중단·Reset 보존, 실제 AUXSPI/DSi/IR 전송과 cold 상태 복원, SaveManager 선행 Flush 후 전체 파일/여분 보존을 구분한다. 상태 제어가 전송 중일 때만14.8이며 실제1.1.75 reader/writer와 헤더 수정 없이 대조한다. 구버전의 이미 반영된 WRSR은 재실행하지 않고, 데이터 전 상태는 이어받는다. 물리WP·쓰기 지연·재삽입 후 보호 상태·Flash 보호 변종은 미검증이며 FRAM의 비정상 추가 바이트 거부는 실기 계약으로 세지 않는다.

## 구버전 이관·통신과 보안 후속

[1.1.31](releases/1.1.31.md)의 `fat-storage-`는 실제 생성 FAT 이미지와 현재 Qt I/O/FatFs에서 마운트 읽기·탐색·길이 오류, 비정상 파일과 새 포맷 실패를 주입한다. 이미지·인덱스·폴더의 바이트 보존, handle 닫기, 같은 이미지 재시도를 확인하며 `cart-replacement-invalid-sd`는 실제 DSi/카트 소유자가 실패 객체를 삽입하지 않는지 검사한다. `core-dsi-btdmp`는 한-word checked STL 반례와 실제 I2S/DSP 호출을 확인한다. 부족 채널 0은 명시한 안전정책이며 실기 oracle이 아니다. Compute 초기화는 기존 frame/capture 세 실행의 전후 출력이 같았고 성능 향상으로 세지 않는다.

호환성 변경은 같은 버전끼리의 성공만으로 완료하지 않는다. 원본/구버전 writer가 만든 일반 게임 저장·설정·중간 저장을 새 reader에 전달하고, LAN은 호스트 역할을 바꾼 연결·게임 패킷 왕복을 확인한다. 포맷 major가 다른 중간 저장을 헤더 수정만으로 허용하지 않으며, 업그레이드와 구버전으로 되돌리기의 지원 방향을 구분한다. [1.1.30 기록](releases/1.1.30.md)은 원본 소스의 설정 이관 성공 및 LAN 클라이언트 방향의 미통과를 함께 남긴다.

후속 보안 점검은 기존 NP-01/02·CJ-10·FS-01/02에 연결한다. 패킷/파일 길이·peer 권한·처리량과 큐의 상한·불량 입력 뒤 복구를 생성 입력으로 확인한다. 통신 지연을 줄이기 위해 검증을 제거하지 않으며, 정상 입력의 처리 비용과 오류 입력의 자원 사용을 별도로 측정한다. 이 문단은 점검 계획이며 보안 감사 완료나 침투 시험 성공을 뜻하지 않는다.


FS-05는 `save-manager-allocation-capture|save-manager-allocation-publish`, `cart-replacement-capture-*`, `frontend-close-capture*`로 확인한다. 호출 스레드의 지정 크기 `new[]`만 실패시켜 최초/성장 버퍼와 보조 버퍼의 실패 전달·기존 파일 보존·최신 데이터 미확보 시 성공 방지·다음 부분 쓰기에서 전체 복구를 검사한다. 실제 코어의 DS/GBA 저장·생성 AP 설정·합성 외부 firmware를 새 게임 쓰기 없이 재확보한다. 종료 창은 실제 추출 메서드·Qt 대화상자와 producer/save 대역을 사용해 일시정지·Retry·SaveCopy 순서를 확인한다. 별도 생성 DS/DSi/GBA 실행은 전체 Qt EmuThread·카트·Platform callback에서 실패 후 다음 프레임 복구를 확인한다. 통제된 할당 실패를 자연 메모리 고갈·실물 chip·장기 경쟁 수락으로 확대하지 않는다.

1.1.72의 `cart-replacement-ds-capacity-`4그룹은 원본 길이·내용을 기준으로 실제 로더/칩 SPI와 SaveManager의 첫 쓰기·후속 wrap/full-chip 쓰기·재로드를 검사한다. 등록 메타데이터와 큰 파일의 패딩, 미지원8MiB 파일의 SPI/NAND 구분, 서로 다른 물리 용량의 상태 복원·프로토콜·후속 section·거부 입력도 확인한다. 현재 writer의 헤더를14.1로 바꾼 검사는 **합성 legacy-layout 검사**이며 구버전 writer 실행으로 세지 않는다. 원본1.1의 실제 게임 상태600프레임 검증은 별도다. 파일 여분은 상태에 직렬화하지 않으므로 상태만으로 해당 여분을 이관/백업한다고 보장하지 않는다.

1.1.73의 DS 수동 선택9그룹은 `cart-replacement-ds-manual-`·`asset-ui-ds-save-`·`rom-preparation-ds-save-`·`ds-save-request-consumer`다. 실제 로더/SPI/SaveManager와 추출한 Qt 선택/큐 소비 검사를 구분한다. 별도 실제 앱6그룹은 QAction→파일/종류 선택→준비→실제 소비를 거치고, 첫 쓰기/재로드·파일 여분·취소/재선택·일반/최근/드롭 기본값을 확인한다. 생성 입력과 offscreen/dummy 장치를 사용하며 물리 장치나 모든 chip 명령의 검증으로 세지 않는다. 전체866/867의 파일 교체 실패와 단독1회 통과, JIT OFF20/21의 잘못된 R4 태그 기대값 및 실제 클래스 검사로 수정한1/1 결과를 각각 보존한다. 같은 성공 검사를 다시 실행해 최초 실패를 지우지 않는다.

1.1.74의 `retail-flash-`4그룹은3용량·페이지 끝/초과·읽기·CS 해제·지우기 주소 정렬/거부·대기 상태와 손상 입력을 확인한다. `cart-replacement-ds-capacity-flash`는 실제 SaveManager의 선행 캡처 뒤 쓰기·지우기·파일 여분·재로드, `cart-spi-flash-state`는 실제 MMIO/scheduler와 DS/생성 DSi·ARM9/7·IR의13경로를 확인한다. 후자는 CTest 출력 상한으로 성공 로그가 잘리므로 검토한 테스트 분기와 프로세스 성공을 근거로 삼는다. 별도1.1.73 소스의 writer/reader 대조는 헤더를 수정하지 않은 실제 구형 프로그램 실행이며, 원본1.1 게임 상태 재생과 구분한다. 초기 Flash4실패, 통합 IR2실패의 원인/수정, 전체872/873 및 기존 Qt10 거부의 단독1회 통과를 보존한다. 물리 chip timing/WIP/WP·전체 콘솔 실패 원자성의 검증은 아니다.

1.1.75의 `retail-eeprom-profile-`3그룹은 명시EEPROM의 페이지 끝/초과·CS 이전 불변과FRAM 즉시/연속 쓰기, 같은 용량의 매체 복원·부분 상태·손상 기록/receiver padding을 확인한다. `cart-replacement-ds-capacity-profile`은 첫 캡처 이후 실제 디스크 보존/재로드와32KiB 자동FRAM 오판 방지, `cart-spi-profile-state`는 DS/생성DSi·ARM9/7·IR의5경로를 전체 콘솔 저장/복원으로 검사한다. 실제1.1.73 writer→현행 exact 수신자, 새 일반14.2→구 reader 및 새 idle/pending14.7 거부는 별도8회 실행이며 합성 헤더 변경이 없다. 칩3/소비자4 baseline 실패, 후보41/41·JIT OFF26/26, 전체877/878 및 FS-06의 단독1회 성공을 각각 보존한다. 이를 전체 칩 명령/WIP/WP·실물 timing·전체 콘솔 실패 원자성의 수락으로 확대하지 않는다.

1.1.77 검증 기록: 최종 Windows SMC69개와 실제far 캐시 소진 후91검사·보존된 interpreter24상태 대조 통과. JIT OFF는SPI13+SMC23=36개 통과. 전체883/884의 유일한 실패는QSaveFile commit 접근 거부이며 단독1회 성공과 최초 실패를 구분한다. SPI 전송 중14.9/실제1.1.76 양방향 허용·거부40회, DS/DSi MMIO 및 개인 입력 보존을 확인했다. native ARM64와 실기 timing은 수락하지 않았다.

1.1.78은 `retail-eeprom-internal-write|internal-state`와 `cart-spi-write-timing`으로 내부 write latch·WIP/WEL·busy 명령·실제MMIO 기한 전/동시/후·저장 통지1회·DS/DSi cold 복원·powered sleep·취소 경계를 검사한다. chip-only 기존 검사는 내부 완료를 명시적으로 전달하며, 실제시간 검사는 production scheduler로만 완료한다. 현재887/887·SMC69·SPI14·JIT OFF44 통과. 보존된1.1.77 소스에서 새2그룹은 실패하며 헤더 편집 없는 전체상태 writer/reader80회가 통과했다. 호환 실행 이후 바뀐 것은 자료 주석/줄바꿈뿐이고 연결한 core는 동일함을 확인했다. 초기 SPI 컴파일 실패 뒤 실행된 구형14개 결과는 제외했다. far-cache는1.1.77 기록이며 이번 버전 재실행으로 세지 않는다. 매체가 불명확한 구형2/3/4·실기/native ARM·모든 게임 수락과 기존FS-06 원인은 남는다.


1.1.79의 `retail-flash-protection|lock|protection-state`는 명시 T9HX3용량의 BP 경계·WL/LD·CS 전후·3ms WRSR·busy·부분/손상 상태를 확인한다. `cart-spi-flash-protection`은 실제 DS/DSi·ARM9/7·IR5경로의 전송/전체상태·전원 취소, `cart-replacement-ds-capacity-flash-protection`은 실제 파일의 보호/해제·chip-end wrap·여분·재삽입을 검사한다. 용량이나 JEDEC ID만으로 T9HX를 자동 선택하지 않는다. 새 타입은14.11 필수이고 실제1.1.78/current 상태82회에서 기존 형식 허용·새 형식의 구 reader 거부를 검증했다.

전체890/892의 두 실패는 기존 invalid15 기대값이다. invalid18로 갱신하고 valid15/16/17을 factory/IR/BT·미지원 가족 검사에 추가한 뒤 영향2/2 통과, 최초 로그를 보존했다. JIT OFF48·SMC69, 실제far reset 후91검사·보존 interpreter24상태 대조가 통과했다. 최종 테스트/배포 식별값 변경 뒤 core·호환 소스/바이너리·게임 입력의 hash 일치를 확인해 성공 결과를 재사용했다. 새 Qt 전체 앱 E2E·물리WP/RESET#·전원 손상·raw sav의 보호 비트 영속성·모든 명령/게임·native ARM·실기 timing의 수락은 아니다.

1.1.80 — `retail-flash-extended-erase|extended-state`는 명시3용량의20/C7 길이·WREN·BP/WL/LD·물리 주소 alias·CS 전후·내부 완료와 잘린 상태를 검사한다. `cart-spi-flash-extended-erase`는 DS/DSi·양CPU·IR10경로에서 queued/held/busy 전체 상태·실제scheduler 기한·reset/전원 취소·통지1회를 검사한다. `cart-replacement-ds-capacity-flash-extended-erase`는 실제SaveManager의 첫 캡처 이후 완료 전 파일 불변·정확한 erase 범위·17바이트 여분·Flush/재로드를 확인한다. 최종 전체896/896·JIT OFF51/51·SMC69/69 통과. 최종 chip 테스트의 실제1.1.79 baseline2실패를 보존했으며, 헤더를 조작하지 않은 실제구형/현행 writer/reader158회는 기존 형식 허용·새 진행14.12 거부·완료14.11 허용을 확인했다. 호환 입력1,508개의 hash 일치로 결과를 재사용한다. 블랙600프레임×3의최종 화면 대조는 애니메이션 전체 픽셀·물리 입력/오디오 지연 수락이 아니다. 새 Qt 전체 앱E2E·물리칩 timing·B9/AB 절전·전체 복원 실패 원자성은 미완료다.

1.1.81 — `retail-flash-power|power-state`는명시3용량의명령길이·WREN 불필요·상태응답거부·busy 무시·보호/lock 유지·raw import·6가지cold 단계와손상/잘린payload를검사한다. `cart-spi-flash-power-state`는실제DS/DSi·양CPU·IR5경로에서controller 대기→held CS→절전/복귀→차단CS 복원, public import의기존deadline 유지·쓰기취소·reset/전원0과배열/통지보존을검사한다. DS는101/1006사이클전/동시를검증하고DSi는프레임경과이므로같은정밀도라고세지않는다. 실제1.1.80/current writer/reader280회는헤더조작없으며기존형식·새14.13거부·완료14.11허용과구형미지원latch/아직전송중인바이트를구분한다.

최종전체898/899의유일한실패는`save-manager-flush-latest`의QSaveFile commit Qt10/Windows 접근거부다. 이standalone target은이번카트core를연결하지않으며SaveManager/fixture는변경하지않았다. 단독1/1통과와최초실패·종료코드8을별도보존하고오류를무시하는assertion이나전체재실행은추가하지않았다. JIT OFF54·SMC69·호환입력1,508개hash일치와블랙600프레임×3최종화면보존을확인했다. 기존FS-06원인·물리retention/timing·전체콘솔복원실패원자성·새Qt전체앱/저장파일E2E는미수락이다.

1.1.82의 `cart-spi-ownership-state`는 ARM9/7·소유권 유지/변경·hold 유무8조건과 느린 A 전송 중 B의 held WREN2조건을 실제 MMIO와 전체DS cold 상태로 확인한다. 원래1.1.81의 확대 재현8 assertion 실패, 초기 후보의 검토 재현10실패, 최종0실패를 각각 보존한다. 최종900/900·OFF55/55·SMC69·SPI18 및실제1.1.81/current reader/writer36회가 통과했다. 구 writer의 취소는 저장 가능했던 소유권 유지 조건이고 새 writer는 소유권을 바꾼다. 양쪽 reader는14.9 그대로 busy/원래기한·mode·owner·byte/CS·실제EEPROM WEL을 확인한다. 전체 중재/실기 timing·DSi cold 경계·전체 복원 실패 원자성 수락은 아니다.

FS-06의 비공개 생성파일 진단은 [Qt6.11.2의 native rename](https://raw.githubusercontent.com/qt/qtbase/v6.11.2/src/corelib/io/qfsfileengine_win.cpp)과 [NtSetInformationFile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntsetinformationfile) 경계를 분리했다. 직접NT 반환값과IO status의C0000022, Win32 오류5, 교체 실패 후 기존 전체 bytes 보존을 기록했다. 실패 직후 삭제 접근 성공/점유자0건은 실패 순간의 원인 귀속이 아니며, 통제한 자체 점유3건은 별도로 구분한다. debugger·필터/ACL·앱 설정·생산 재시도 정책을 변경하지 않았다.

1.1.83 — 실제 Qt 설정창6조건으로32프레임 저장/새 프로세스 재로드,64미리보기/취소,실패/실패한 취소,비정규33설정 복귀,필터OFF→20kHz→하향/취소와 다중 인스턴스 제약을 확인했다. 창의 장치 결과는 mock이며 별도 `FrontendAudio`는 실제 SDL dummy의32/64재개방·일시정지·실패 복귀·양쪽 open 실패·복구·rate 변경·콜백 재개를 구동한다. 새 message 검사는 그래픽 실패/실행/일시정지/콘솔 없음에서 설정 전달과 실행 상태 보존을 확인한다. 실제 Windows 무음 WASAPI 측정은32/64가 약10ms burst 안에서 반복 호출됨을 보였으며 물리 출력/청감 지연 측정은 아니다.

최종 통합은 한 번 실행해906/907. `save-manager-retry-rename`의 통제 점유를 해제한 뒤에도Qt10 접근거부가 발생했고 단독1회에서는 통과했다. SaveManager/해당fixture는 변경하지 않았으며 기존FS-06원인은 미해결로 유지한다. 통합전체 반복·assertion완화는 하지 않았다. 저역통과 필터의 계산생략 후보는64프레임에서 빨랐으나 기본512/OFF에서6.6%느린 측정이라 제품에 채택하지 않았다. far cache 소진은 변경 전1.1.82코어로 실제far reset1회·4079블록·91검사·인터프리터24상태 일치가 확인됐고, 이번오디오변경의 JIT성능으로 세지 않는다.

1.1.84 — Windows 최종 통합910/910, 74.05초, 전체 실행1회. Qt 설정9조건은 출력 tuple의 적용·취소·실패복귀·사용불가 ID 보존을 검사하되 장치 API는 mock이다. 실제 AudioOutput 장치 실행은 무음 preflight가 SPU callback을 호출하지 않음, stop/close 이후 callback 종료, 재개, 두 WASAPI 공유 스트림의 동시 전달, 잘못된 endpoint 이후 재개방을 확인했다. 기본 장치의 IAudioClient3 최소주기는480/48kHz, 다른 연결 장치는48/48kHz였고 후자의 요청32는실제48로 조정됐다. 실제 게임의 SPU·생산 callback·새 출력 모듈을 연결한 별도 headless fixture에서 블랙360프레임씩 SDL512/WASAPI128의 최종 화면 hash가 같고 코어 출력 폐기0, 원본 개인 입력은 불변이었다. 짧은 공급부족은 각각449/417프레임으로 남았으며 약6초의 무음 제출 결과를 장기 무잡음·청감·물리 지연·전체Qt실행·방송/장치 재연결 증거로 확대하지 않는다. 보간 Q3의21개 합성조건은 별도1.1.83 SPU 관측 프로토타입이며 선택 image 감소, 기존guest/capture/output보존을 확인했으나 timer 변경 adapter 누락과 채널 지연 차로 제품에 채택하지 않았다. 이번 전체검사의 FS-06 통과도 기존 간헐 문제 해결을 뜻하지 않는다.


1.1.85 — Windows 전체 빌드·CTest918/918(79.80초, 전체1회, 실패/skip0). R3 실제 변환의 입력 block64/1024 및 동적 속도 변경 PCM 일치, 큐 포화/동시 callback 순서, 합성440Hz 오차5cent 이내, reset 뒤 잔향0을 확인했다. 실제 frontend 상태 로드에서는 성공 시 변환 이력 초기화·실패 시 보존을 확인했다. Qt 설정11조건은 장치 mock이며 화면 배치도 확인했다. 최종 라이브러리로 연결한 실제 게임360frame의 기존/신규1x·2x·0.5x headless 출력은 화면 동일·core drop0이지만 신규 경로는 시작을 포함한 부족3200/2544/4224frame 및 첫 비영 callback 이후896/368/512frame이 남았다. WASAPI128/48kHz 무음 제출은 청감·물리 지연·전체Qt배속 조작·장기/ARM 검증을 대신하지 않는다. AD-08 고품질 채널 보간은 미완료다. GitHub Actions·macOS 빌드는 사용하지 않았다.


1.1.86 — Windows 전체 빌드·CTest919/919(98.00초, 전체1회, 실패/skip0). NP-18은 영향을 받는 기존 운영 소스5개의 hash 일치를 확인해 원 실패 증거를 재사용했다. 생성 카트리지 NAND와 실제 MMIO/scheduler의 가져오기/live/cold·일반 pending/cold·실패한 core 상태 load 이후 백업 복구5조건에서 기대 배열과 전체 바이트가 일치했다. 집중9/9도1회 통과했다. 실제 카트리지·UI 가져오기·DSi 내부 NAND 전체 프로토콜·실기 수락으로 확대하지 않는다. r8brain 최소위상 실험은 별도 private native 후보이며 제품 SPU를 바꾸지 않았다. GitHub Actions·macOS 빌드는 사용하지 않았다.


1.1.87 — Windows 최종 빌드·CTest919/919(91.23초, 전체1회, 실패/skip0). 실제 WASAPI 무음 장치 검사에서 preflight의 client 호출0·일시정지/재개/종료·동시2출력·잘못된 장치 이후 재개방과 진행 중 callback을 유지한 idempotent Start/Stop 대기를 확인했다. 무음 headless 블랙360프레임×요청128/256/512, 각100ms 일시정지3회에서 기존5281/5761/7233→최종321/2027/1025 공급 부족과 코어 PCM 폐기0·동일 최종 화면·개인 입력 보존을 확인했다. 재개 첫 callback부터20ms의9구간 부족0은 실제Qt설정창·청취·물리 지연 증거가 아니다. 최종 버전 증가 전 동일 오디오 라이브러리의 게임 결과를 재사용했으며, core archive는 버전 문자열을 포함한NDS.cpp 재빌드로 달라져 동일 binary로 세지 않는다. 시작/실행 중 부족·장치 상실/재연결·ARM·고품질 보간은 미완료다. 공유 스트림이 열린 동안 일시정지 무음을 유지하며 Close에서 해제한다. GitHub Actions·macOS 빌드는 사용하지 않았다.


1.1.88 — Windows 최종 빌드·CTest919/919(102.16초, 전체1회, 실패/skip0). private main에 설정·타이머 제어를 추가해 실제 Qt EmuThread·설정창·소프트웨어 표시·상태 복원·WASAPI 경로를 실행했다. 원본 BIOS/펌웨어/NAND/ROM/state는 복사본으로 분리하고 원본 hash 보존을 확인했다. 이전128 실행에서 설정창 사이 PCM 폐기7이 발생했고 후보128/512 각3회 설정창 열기/닫기는 폐기0, 첫 설정창 이후 추가 부족0이다. 시작/상태 로드 부족은 남았고 음량0·offscreen이므로 청취·실제 표시/입력 지연·장기 수락은 아니다. 최종 표시 버전만 올리기 전 후보 동작 검증이며 실제 EmuThread 객체를 사용했다. 초기 실행기 private-slot 호출 컴파일 오류와 ROM 연결 확인창이 열린 상태에서 NDS 없이 상태를 로드한 crash는 실행기 오류로 보존했다. 파일 배치를 바로잡은 성공과 구분한다. 이전 headless 실행기의 최초 시간 기준 차이는 제품 수정 없이 별도 교정했으며 공급 수치 개선을 제품 성능으로 세지 않는다. GitHub Actions·macOS 빌드는 사용하지 않았다.


1.1.89 — Windows 최종 빌드·CTest919/919(88.65초, 전체1회, 실패/skip0). 실제 AudioOutput에 등록된 SDL callback을 수동 구동한 정지/재개 회귀는 필터OFF/20Hz/6kHz×16/128/512의9조건에서 기존 최대1000sample 불연속9실패→최종21sample로 감소했다(입력 진폭1000). SDL 초기화를 빠뜨렸던 최초 실행기 실패는 별도로 보존하고 수정된 baseline만 근거로 쓴다. 별도 native SDL/WASAPI의 진행 중 callback·반복 Start·Stop 대기·재개/Close 검사가 통과했다. 최종1.1.89의 실제Qt/EmuThread/설정창·Playback 1/2·양 backend×128/512의4조건은 PCM 폐기0, 각3회 정상 재개였으나 SDL128은 첫 설정창 이후219frame 부족이 남았다. 다른3조건은 해당 구간 추가 부족0이며 시작/상태 복원 부족은 유지된다. 무음/offscreen 실행을 청취·물리 지연·장기/ARM/장치 상실 수락으로 세지 않는다. GitHub Actions·macOS 빌드는 사용하지 않았다.


1.1.90 — 최종 Windows 앱 빌드 성공. 오디오 설정11조건과 callback1조건은12/12(3.00초); SDL zero-name 계약 보정 후 영향 callback만1/1(0.60초)로 재검증했다. 최종 UI tooltip 변경은 새 테스트 없이 실제 Qt 창으로 확인했다. runtime 조회 실패가 저장된 WASAPI를 계속 선택 가능하게 하던 기존 동작은 확대 UI 검사1실패로 재현했다. 정상·열거 실패·endpoint 없음·장치 재등장·설정 보존/취소/실패복구는 Qt 설정 테스트에서 장치 facade를 대체해 확인한다. 별도 실제 SDL/WASAPI 열거는 각각 기본값 포함10항목 및 Playback 1/2를 확인했고, WASAPI context 초기화/열거 실패/빈 endpoint 주입3조건은 빈 목록과 오류, 이후 정상 열거 복구를 확인했다. 실제 production 객체를 연결한 Qt 앱2실행에서도 정상 선택 및 사라진 저장 장치의 비활성·다른 기본 장치 선택·취소 보존을 확인했다. 개인 설정 대신 private portable을 사용했으며 ROM 실행·청취·물리 지연/탈착·다른 OS 검증은 아니다. 새 전체 suite는 반복하지 않았다(이전1.1.89의919/919와 구분). GitHub Actions·macOS 빌드는 사용하지 않았다.


1.1.91 — Windows 최종 전체 빌드 성공. 기존 실제 SPU/I2S/ReadOutput에서 일정 PCM16을32→47→32kHz로 바꾸거나 같은 rate를 다시 설정한4조건(출력44.1/48kHz)은 수정 전 exit8·최대2348/2308/2309sample 차이를 재현했고, 수정 후 rate 변경 없는 동일 guest 시간의 전체 PCM과 byte-identical이다. 초기 실행파일 경로 오입력은 실행 실패이며 그 뒤 실제 root 실행파일의 exit8을 baseline으로 보존했다. 최종 전체 CTest는918/919(84.82초, 전체1회). 유일한 save-manager-unpublished-pending 실패는 QSaveFile commit의Qt10/Windows 접근거부이며 해당 target은Qt Core만 연결하고 변경한SPU를 연결하지 않는다. 해당 소스는 미변경이고 단독1/1(0.15초) 통과했으나 기존FS-06 원인은 미해결로 유지한다. 실패 기록·assertion은 보존하고 전체 검사를 반복하지 않았다.

AD-08 private 출력 연결은 기존1.1.90 기반14개 DS/DSi 조건에서 원래SPU/capture/I2S/DSP 상태 보존과 DSP-only/음소거 PCM 동일성을 확인했다. 일부 DSP 혼합 조건은 실제DSP clock 뒤 명시된 합성PCM을 주입하므로 게임DSP 음질 수락으로 세지 않는다. 별도1.1.91 후보의32/47kHz 교체 조건에서도 새 보간의DSP-only PCM과 수정된 기존 출력이 일치했다. 고품질 신호의 추가 지연·계수 배포·옵션·전체 이력 상한·실제게임/장치/ARM은 미완료다. 이 버전은 rate 전환 수정만 배포하며 GitHub Actions·macOS 빌드는 사용하지 않았다.


AD-08 미배포 소유권 후보 — Windows AudioResampling/core 빌드와 기존 audio-core-clock/one-shot-hold/one-shot-state 3/3(2.97초). 새 전체 suite는 반복하지 않았다. private 실제 디코더 6조건(352/512, 전체 format의 timer 변경/재시작 및 긴 support)의 동일 PCM/게스트 hash를 유지하며 16채널 이력 예약36,741,120→23,230,464byte, 처리 중 new0/PCM 폐기0을 확인했다. 이는 bank와 전체 앱 메모리를 제외하며 관측 peak로 상한을 결정하지 않았다. 실제 scheduler의 rate 전환에서 후보 exception을 재현하고 수정한 후 DSP-only PCM·게스트 SPU/I2S/DSP 상태·24,167 DSP clock이 기존 코어와 일치했다. 최초 비교기는 stderr 합친 로그와 stdout을 섞어 실패했으며 기록된 JSON stdout을 사용해 실행 반복 없이 정정했다. 제품 core에 연결한 소유권 후보도 동일 결과였다. 별도 DS/DSi 실제 NDS state transfer에서 MIC 섹션 오류가 SPU를 부분 변경한 것을 확인한 뒤 백업 복구/host 이력 유지, 성공 load 후 reset, 네 개 동시 소유 인스턴스, legacy↔quality 전환,44.1kHz 변경을 확인했다. 인스턴스는 순차 실행이며 멀티스레드/UI/실제 장치·ARM 수락은 아니다. 성공 load와 mode-switch는 동일 초기 조건의 쌍 비교로, 별도 실기 음질 oracle이 아니다.


1.1.92 — 제품 내장 계수와 실제 core를 쓰는 audio-minimum-phase 및 설정 UI3조건, 총4/4(2.47초) 통과. DS/DSi rate scheduler, guest SPU/capture 보존, 부분 state 실패 뒤 backup 복구, mode 중복 설정 무할당·준비 실패·새 renderer 생성자 실패 전달을 확인했다. 초기 일반 next-allocation 생성자 주입은 기존 noexcept JIT 할당에서 종료코드3221226505로 실패했으며 보존한다. 새 renderer 크기만 겨냥한 별도 주입의 성공이 그 기존 문제 해결을 뜻하지 않는다. 실제 Qt/EmuThread+블랙/상태+Playback1/2의 WASAPI 요청128(실제480)·512(실제512) 각12초에서 고품질 미리보기·기존3/5로 취소·수락/저장 및 다른 설정 후 유지가 통과했고 core PCM 폐기0, 개인 입력 hash 유지였다. 누적 부족은1069/899frames로 남았다. offscreen·음량0이며 청감·물리 지연 수락은 아니다. 두 설정창 캡처의 설명/옵션 표시를 확인했다. 계수 원본57파일은 r8brain7.5 고정 commit archive와 모두 byte-identical; bank hash/입력 chain 확인과 독립 재생성 가능 여부는 구분하며 중간 metadata writer 부재를 공개했다. 최초 통합 빌드는 제품 메서드를 추출하는 CartReplacement fixture의 새 renderer include 누락으로 실패했고, include 보완 뒤 전체 빌드가 통과했다. 전체 CTest는1회922/923(90.83초); 유일한 실패는 WindowsDeploy의 예상 목록에 새 r8brain 라이선스가 빠진 것으로, 목록 및 원본 hash 일치 검사를 추가한 뒤 영향1/1(5.24초) 통과했다. 최초 실패들은 보존하고 전체는 반복하지 않았다. 최종 test 변경은 오디오 실행 코드에 영향을 주지 않으므로 실제 게임 성공 결과는 재사용한다. FS-06은 이번 전체에서 재현되지 않았으며 원인은 계속 미해결이다. GitHub Actions·macOS 빌드는 사용하지 않았다.


1.1.93 — 기존 FrontendAudio의 실제 device adapter/추출 audioEnable에서 빈 source를 즉시 소비하는2실패를 재현했고, 수정 후 같은 검사와 첫 PCM 전달·후속 underrun 기록 검사가 통과했다. 초기 영향8개 중7개가 통과했으며 R3 성공 load 검사는 빈 callback을 기다리다 실패했다. 새 계약에 따라 source가 없을 때 시작 예약/정지를 확인하고, 실제 코어/R3가 최대16 source frame 안에 PCM을 내는지 확인하도록 보완했다. 이후 상태7/7(2.75초) 통과; assertion을 침묵 PCM으로 대체하지 않았다. 실제 Qt+블랙+Playback1/2 요청128(실제480)/512(실제512) 각12초·설정 열기/닫기3회에서 core 폐기0, 개인 입력 hash 보존. 일반 보간의 빈 시작은 약8.5~8.7ms/로드 후3.7~4.1ms 생산 대기를 확인했고, 기존 큐 재개는0.10ms 안팎이다. 부족 누적은 별도 baseline908/1283→후보53/551frames였으나 기기 callback phase가 달라 성능 비율로 일반화하지 않는다. R3 ON 별도12초는 첫 준비51.3ms/로드36.6ms, 부족2011frames가 남는다. first fragment가 callback보다 짧고 실행 중 부족도 있으므로 틱 해소나 물리 저지연 달성을 선언하지 않는다. offscreen·음량0이며 청취·장기/다른 OS 수락은 후속이다. 최초 private 타이밍 fixture의 문자열 escape 컴파일 실패도 보존했다.

최종 Windows GUI/설정/메시지 target 빌드와 추가 통합21/21(4.32초) 통과. 이미 통과한 callback과 상태7개를 반복하지 않았고, 전체 core suite는 변경 없는1.1.92 결과를 현재 재실행으로 세지 않는다. 새 runtime은 이전 manifest의 byte-identical321파일을 재사용하고 새 EXE/버전/안내/launcher4파일만 교체했으며 현재 SDK 원본·129 PE import·clean PATH의 help/build-info/launcher를 다시 확인했다.


1.1.94 — [Microsoft IAudioClient3 계약](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream)의 fundamental 배수/min/max 조건을 적용했다. 같은 Playback1/2의 직접 조회와 라이브러리 debug 기록에서128은IAudioClient3/480, 기존512는fallback/512를 확인했다. 각1초 무음 cadence의512 p95/max는19.994/20.012ms→10.072/10.091ms, native capacity1536→1056frames였다. 이는 물리 출력 지연 측정이 아니다. 다른 장치의 현재fundamental1/min48/max480에서 요청32→48,128→128,512→480을 확인했다. 이 장치는1ms 지원이어도 host wakeup 변동이 있어 보편적1ms 안정성을 주장하지 않는다. 보수 profile의512→512 fallback을 유지했고, private library 복사본에서만 QueryInterface/GetSharedModeEnginePeriod/InitializeSharedAudioStream 실패를 각각 주입한9개 실제 fallback open/start/stop이 통과했다. 공개소스에 실패 주입은 없다.

변경 target Windows 빌드와 기존 callback/UI15/15(3.91초) 통과. 전체 core suite는 반복하지 않았다. 실제 Qt/DSi 블랙·state load·설정 열기/닫기3회 및 Playback1/2 요청128/512 각12초에서 실제 callback480/480·native capacity1056/1056, PCM 폐기0·개인 입력 hash 보존이었다. 부족은161/2frames로 남았고 별도 세션의1.1.93 결과를 통계적 성능 비율로 사용하지 않는다. 마지막 설정 화면에서512 요청과480 실제값을 함께 확인했다. offscreen·음량0이며 장기 청취·물리 입력/출력 지연·방송 E2E/native ARM은 미검증이다.


1.1.95 — 실제 Qt/DSi 블랙/Playback1/2 요청128(실제480), R3 ON, load·설정 재개3회·12초의 frame/부족 시각을 메모리에 기록했다. 기존60FPS는 중복 FPS 대기144프레임·부족1751frames/13회, 후보는 대기0·첫 시작227frames/1회였다. 실제 마지막2초 속도60.098→60.099FPS, 출력 큐 최대1819→1818, 중앙값545→555로 한도 증가 없이 비교했다. 첫 PCM 준비51.0→33.8ms, load37.2→11.5ms이며 물리 출력 지연은 아니다. 후보30FPS는30.15FPS·부족0,15FPS는15.003FPS와 기존 대기를 유지했지만 오디오 속도 하한으로 큰 부족이 남는다. sync OFF 후보/원본 모두 약60FPS·PCM overflow를 관측해 신규 회귀와 구분했다. 최초 guard 묶음의 무조건 무손실 assertion은 sync OFF에서 실패했고 그대로 보존했다; 이 경로의 손실을 통과로 재분류하지 않았다. 비R360FPS도 기존 제한을 유지한다. 모든 실행은 offscreen/음량0·개인 원본 hash 보존이며 청취/방송/물리·다른 OS 수락은 후속이다. Rubber Band 보정량에 대한 agent의 ratio 산술은 근거 부족으로 철회되어 코드에 반영하지 않았다. 전체 core 검사는 반복하지 않았다.


1.1.96 — 기존 정상 속도/R3 OFF 블랙 trace에서 부팅3번째 frame17.4ms 처리 중239/480만 공급했고, 직전 frame들의 중복 FPS 대기는8~9ms였다. 실제 Qt/DSi 블랙·R3 OFF·Playback1/2 요청128의 WASAPI(실제480)와 SDL(실제128) 각12초 후보에서 공급 부족0·PCM 폐기0, 마지막2초60.099FPS·출력 큐 최대1600을 확인했다. 큐 한도/폐기 정책 변경은 없다. SDL callback128도 native10ms 안팎 burst로 전달되므로 물리 지연128/48000초로 주장하지 않는다. 실제 모드 toggle field를 사용하는 별도12초에서 일반59.50→저속29.98→배속119.94→일반60.20FPS와 저속/배속 FPS 대기 보존을 확인했다. 기존 배속은 audio backpressure를 우회하므로 이 검사는 배속 PCM 무손실 증거가 아니다. 개인 원본 hash 보존, offscreen/음량0 조건이며 장기 청취·다른 장치/OS/물리 지연은 미검증이다. 변경 없는 core 전체 검사는 반복하지 않았다.

1.1.97 — LocalMP 기존1.1.96의 두 host CMD 교차 실패와 후보의 전원 유지 host→client 실패를 각각 재현 후 수정, 생산 queue/실제 mutex·semaphore17조건 통과. GPU 계측 초안의 query-name/index 혼동을 재현해 독립 timestamp 쌍으로 수정했으며 disabled/reuse/nesting/exhaustion/async/recreation과 실제 GL/Compute frame capture3검사가0.82초에 통과했다. 블랙 DSi/HLE 동일 state300프레임 Classic/Compute에서 각 renderer의 계측 OFF/ON 전체 PPM hash가 일치하고 원본 입력은 보존했다. Classic3.2는 GPU 미지원, Compute는 비동기 결과와drop0 확인. 처음 LLE 재생기의 HLE state 거절은 설정 일치 후 해소했으며 검증 조건을 완화하지 않았다. 실제 Qt GDB 포트 중복은 설정 변경 없이 대화상자 유지, 수정 후 저장·GDB 비활성의 동일 포트·JIT 배제도 확인. Windows 변경 target 빌드 통과; 전체 suite는 반복하지 않았다. 실제 네 게임 인스턴스 두 그룹/Qt GL present·다른 GPU/장기 청취·물리 지연/실제 GDB bind 및 종료 수명은 별도 잔여다.

1.1.98 — AD-04 장치 상실 복구: 기존 SDL stale running을 재현한 뒤 NeedsRecovery/동일 설정 재오픈으로 수정했다. 집중16검사, 실제 Qt fault seam(장치 상실→저장 출력 재시도→죽은 핸들 재오픈), 자체 WASAPI 스트림 정지 복구가 통과했고, 실제 Qt/DSi 블랙 Playback1/2 요청128(R3 OFF)의 WASAPI(실제480)·SDL(실제128) 짧은 실행에서 공급 부족·PCM 폐기·코어 drop이0이었다. Config 선호 보존, 설정창 열림 중 보류, 5초 상한 백오프를 함께 확인했다. 물리 케이블 분리·장시간 청취·장치 재라우팅과 드라이버 Open/Close 지연은 미검증이며, 복구가 UI 스레드를 동기로 막는 한계는 남는다(비블로킹 보장 없음).

1.1.99 — 실제 SPU::BufferAudio/blip 생성 stereo 6조건×60000호출, 교차 A/B3회에서 PCM hash/프레임/폐기 개수 일치와 함수 중앙값15.3~18.9% 감소를 확인했다(호출당 수십~백여 ns이며 게임 전체/물리 지연 이득 아님). 실제 DS/DSi 3프레임 소비 지연·overflow·reset의 최신2047프레임 PCM도 불변. 실제 TCP 중단/재접속2세션의 idle nonblocking poll은225757→0이며 재개·닫힌 listener 종료를 확인했다. 관련12검사 통과; 전체 suite 반복 없음. 실제 Qt/DSi 블랙 Playback1/2 요청128·R3 OFF의 짧은 SDL128/WASAPI480 재생에서 공급 부족/코어 폐기0 및 원본 hash 보존. offscreen/음량0 조건이며 청취·물리 지연·다른 OS·UI 중단 CPU 취소와 bind 오류 전달은 미검증/후속이다.

1.1.100 — 새 firmware-retain 회귀가 수정 전 실패하고 수정 후 DS/DSi 거절 시 코어·BIOS·삽입/대기 카트·RAM·실행 상태 보존을 확인했다. 실제 Qt frontend의 소유 ARM 루프에서도 실패 뒤 BIOS/JIT 변경(state_retained=0,jit=1)이 보존(1,0)으로 바뀌었다. 관련 DS/DSi 부팅·리셋·카트 12검사 통과, 전체 suite 반복 없음. 생성 NAND의 정상 메뉴 준비 검증은 실제 펌웨어/게임 부팅 또는 다른 OS 검증을 대체하지 않는다.

1.1.101 — 실제 Qt에서 점유된 TCP 포트로 GDB ARM9 초기화 실패를 재현했고, 수정 전 reported=0에서 수정 후 reported=1로 바뀌었다. ARM7 listener와 실행 상태는 유지했고 포트 해제/재시도 후 두 listener 정상·경고 없음 확인. GDB 실제 loopback/닫힌 listener 관련 2검사 통과. OSD 큐를 확인한 소유 fixture이며 다른 OS/실기/전체 GDB 제어 지원 완료 증거는 아니다.

1.1.102 — 실제 제품 오브젝트의 Qt 시작 중단/undefined 예외 재개/실패 리셋/DS→DSi 동일 포트 교체/프레임 스텝+state roundtrip/중간 snapshot 거절/handshake/control 8시나리오 통과. Compute 및 Legacy 다중 창 재생성은 제품 GdbStub에 관찰 호출만 넣어 child의 GL 복귀와 원래 실행 위치를 확인했다. 실제 GdbFrame의 native Fiber 및 libco 어댑터는 Windows에서 2스레드·512yield·task/host 예외·취소·FP환경 통과. GDB OFF의 frontend 2TU×JIT ON/OFF 컴파일 통과(별도 완성 바이너리/다른 OS 실행 아님). 영향 경로12검사와 deadline 등5개 protocol 검증 통과; 중첩 frame-step pause의 오디오 정지 누락을 추가 회귀로 재현/수정 후 그 검사만 재실행했다. 전체 suite 반복 없음.
최종 diff에서 RunFrame 분기 재배치 중 빠진 치트 오류 OSD 소비를 복원했다. 생성한 unsupported-code 오류를 실제 Qt GDB 프레임 완료 경로에 전달해 수정 전 안내0/미소비, 수정 후 안내1/소비 완료를 확인했다(사용자 치트·게임 파일 미사용).

1.1.103 — 생성 SDL 장치의600ms Open 지연을 실제 Qt 복구 타이머에 넣었을 때 UI heartbeat 최대606ms→11ms. 생성 ARM 루프의 복구 중 프레임 진행·stale 설정 교체(UI gap19ms)·open 도중 종료/자원 drain을 확인했다. 기존 Qt 복구·fallback·설정 선호 보존·백오프 및 Playback 1/2의 자체 WASAPI 스트림 stop/reopen도 통과했다. 집중3검사 후 async owner에 close/pending teardown을 추가해 해당1검사만 재실행, 설정 취소/실패4검사 통과. 정상 게임 FPS/물리 오디오 지연 개선 또는 실제 장치 탈착의 증거는 아니다. SDL2.32.10 WASAPI의 Open/Close 동일 스레드 규칙과 bundled miniaudio COM 수명을 유지했다.


1.1.104 — 느린 백그라운드 세이브의 파일 I/O가 StateLock을 통해 매 프레임 오디오 생산까지 막는 경로를 제거했다. 저장 바이트를 잠금 안에서 확보하고 파일 writer를 별도로 직렬화하며, 완료 시 해당 경로·버전만 확인한다. 실제 Qt/DSi 블랙에서 저장에80ms 지연을 주입한 전후 비교(각12초, 지연 전후100ms 관찰)에서 공급 부족은 WASAPI 실제480프레임2561→0, SDL 실제128프레임2369→0이었다. 요청 버퍼128/R3 OFF/Playback1/2를 유지했고 두 실행의 저장 바이트 hash가 일치했다. 저장 경로 변경·동시 새 publication·메모리 부족·쓰기 거부/재시도를 포함한16검사 통과, 전체 suite는 반복하지 않았다. 계측 뒤 출력을 무음화한 통제 재현이며 자연 발생한 모든 틱의 원인 귀속·청취/물리 지연·다른 OS·FS-06의 Windows 교체 거부 원인 해결은 아니다.


1.1.105 — File → ROM library에서 명시 선택 폴더를 기억하고 DS/Zstd/지원 archive 파일을 필터링·정렬한다. QFileSystemModel의 비동기 목록/변경 감지를 재사용하며 ROM 바이트/멤버 캐시는 만들지 않는다. 실제 위젯 검사에서 선택 파일 삭제 시 Qt가 옆 ROM을 자동 선택하는 문제를 재현하고 선택 해제로 수정. 추가/삭제·Unicode/대문자·폴더 재선택/누락·Enter/더블클릭1회·취소 검사를 통과했다. 마지막 실제 루트 삭제 검사에서 캐시 항목/열기 활성 상태가 남아 실패했고, 선택 루트 소실 감지와 무효 루트의 열기 차단으로 수정했다. 실제 Qt 전체 앱의 생성 DS 루프에서 목록 표시 후 변경한 RAM marker를 읽었고, 두 멤버 ZIP에서 선택한 멤버 실행/최근 목록 identity·폴더 설정 저장·창 재사용/종료를 확인했다. 초기 실험 ROM의 secure-area 배치 오류는 입력 생성기에서만 수정했다. offscreen Windows/Qt6 검사이며 실제 느린 매체의 OS 호출 중단·다른 OS/Qt5·재귀 탐색과 메타데이터 카탈로그는 검증/구현 범위 밖이다. 기존 오디오/코어 전체 suite는 반복하지 않았다.

1.1.106 — 동일 ROM 폴더 삭제·재생성/즉시 재선택의 오래된 목록을 실제 파일·위젯으로 재현하고 QFileSystemModel 교체로 수정. 새 파일명·경로·바이트와 기존 동작을 포함한 rom-library-live-folder 1/1 통과. 별도 offline 생성기는 고정 r8brain archive/57파일을 검증하고 두 계수를 새로 fit한 후 제품 loader로 비교: Windows GCC16.2/NumPy2.4.2에서 기존352/512 bank와 각각 SHA256 일치, moment/step 차이0. 원본 source 변조·기존 출력 경로 거부 확인. 기존 출력 거부 fixture의 영문 OS 오류 가정은 Windows183/기존 bank 해시 보존 검사로 정정했다. 제품 계수·오디오 runtime은 변경하지 않았으며 역사 metadata writer 복원·다른 BLAS bit identity·물리 음질/지연 수락은 별개다. 전체 suite 반복 없음.

1.1.107 — 고품질 보간 Step/GetRecord의 산술·배열·이력 용량은 유지하고 인라인 호출로 전환했다. DS/DSi 실제 scheduler의1/16채널·고정/가변timer 8조건×교차3회에서 PCM+게스트 상태 일치. Black/DSi/HLE 동일 상태에서360프레임×교차3쌍의 frame 중앙값4.4~10.8% 감소, PCM·최종 화면 동일, 개인 입력 hash 보존. 처음의 LLE/HLE 상태 불일치는 fixture 설정을 저장 상태와 맞춰 정정했으며 제품 상태 검증을 약화하지 않았다. SIMD gate 병렬 수정의 영향을 배제하도록 수락 A/B는 기존 Math/Accumulate 헤더를 고정했다. 반복 나눗셈 제거 후보는 일관 이득 없어 기각. SIMD OFF인데 선택 커널이 컴파일되는 실패를 재현 후 core PUBLIC gate로 수정. 관련3/3 통과; 별도 실제 CMake OFF의 core/consumer 정의 동일 및 생성 명령으로 consumer 실행 확인. OFF 전체 core 재빌드·다른 OS/ARM·물리 지연/틱 해결은 주장하지 않는다. 전체 suite 반복 없음.

1.1.108 — GCC core PGO GENERATE→seal→별도 USE 도구 구현, 기본 OFF. 실제 GCC fixture에서 source/compiler/compile flag/data 변경 거절과 학습 밖 입력 출력 일치 확인. Windows GCC16.2·LTO OFF에서 core86/93 TU 수집, PGO-use 관련5/5 통과. Black/미학습 Solatorobo 각1200 DS boot frames×교차3쌍에서 PCM·최종 화면 동일, 개인 ROM/save hash 보존. Black 중앙값11~16% 감소지만 Solatorobo 일부5~8% 및 tail 악화로 해당 profile 기본 배포 기각. 측정 뒤 Python3.11 호환 테스트 cleanup만 수정하고 해당 GCC fixture 재통과; 기존 profile은 이전 source identity 자료로 보존하며 변경된 트리에 재사용하지 않음. ROMSmoke PCM 신규 출력·기존 파일 덮어쓰기/장치 동시 소비 거절 확인. 다른 OS/native ISA·Clang·실제 장치 지연/틱 해결·전체 게임 개선은 미검증. 전체 suite 반복 없음.

1.1.109 — ARM9 금지 LDR post의 목적/base 변경과 warmed native 권한 우회를 실제 guest로 재현 후 단일 전송에 수정. 접근 성공 여부를 명시 전달하고 실패한 JIT trace는 발행하지 않는다. native 단일 전송은 현재 MPU 권한을 검사하며 예외 이전 dirty 레지스터/flags를 보존한다. ARM/Thumb20종의 cold deny→allow→warm deny→allow 및 CPU/CP15 저장복원, 관련15/15 통과. 정상 Black 실행이 드러낸 x64 rel8 분기 거리 회귀는 긴 조건부 분기로 수정; 예외 코드는 far 영역으로 이동. 두 DS 게임1200프레임×교차3쌍 PCM·화면·개인 입력 hash 동일. 최종 통합27/27 통과. 최종 frame 중앙값은 Black +0.3~4.5%, Solatorobo +0.8~2.8%; 이전 far 배치의 더 낮은 수치를 최종 성능으로 쓰지 않는다. 정확성 보완 비용을 기록하며 전체 성능 향상·물리 지연 해결은 주장하지 않는다. SWP/다중/이중 전송·강제 user 접근·실기 timing/native ARM 장치 수락은 남았다. ARM64에도 같은 guard를 구현하고 기존 생산 emitter 추출→Unicorn64조건의 권한 판단·dirty spill·helper 입출력·CPSR/cycle 복원 확인; 실제 native ABI/전체 메모리 emitter 실행의 대체는 아니다. MPU 활성 map 포인터는 ARM64 immediate 범위에 배치하되 상태 형식은 유지했다. 전체 suite 반복 없음.


1.1.110 — SWP/SWPB의 읽기 실패 후 쓰기·중복 abort, 쓰기 실패 후 Rd 변경, warmed JIT fallback 이후 명령 실행을 수정했다. ARM DDI0100I A4-213/215의 접근 실패 규칙을 적용하며 예외 벡터로 채운 pipeline을 기존 drain/exit 경로로 처리한다. 수정 전 interpreter/fastmem에서 추가20조건 중8실패 재현. 최종 기존 MPU 검사 확장109조건×3경로(예외 복귀·재시도15건 포함), 관련18/18 통과. Black/Solatorobo 각1200 DS boot frames의1.1.109 대조에서 화면·PCM 동일 및 개인 ROM/save hash 보존. 단일쌍 출력 대조이며 성능 개선·실기 abort timing·native ARM64 수락은 주장하지 않는다. 다중/이중 전송과 강제 user 접근은 후속이다. 전체 suite 반복 없음.
