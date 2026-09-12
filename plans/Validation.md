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

## 구버전 이관·통신과 보안 후속

[1.1.31](releases/1.1.31.md)의 `fat-storage-`는 실제 생성 FAT 이미지와 현재 Qt I/O/FatFs에서 마운트 읽기·탐색·길이 오류, 비정상 파일과 새 포맷 실패를 주입한다. 이미지·인덱스·폴더의 바이트 보존, handle 닫기, 같은 이미지 재시도를 확인하며 `cart-replacement-invalid-sd`는 실제 DSi/카트 소유자가 실패 객체를 삽입하지 않는지 검사한다. `core-dsi-btdmp`는 한-word checked STL 반례와 실제 I2S/DSP 호출을 확인한다. 부족 채널 0은 명시한 안전정책이며 실기 oracle이 아니다. Compute 초기화는 기존 frame/capture 세 실행의 전후 출력이 같았고 성능 향상으로 세지 않는다.

호환성 변경은 같은 버전끼리의 성공만으로 완료하지 않는다. 원본/구버전 writer가 만든 일반 게임 저장·설정·중간 저장을 새 reader에 전달하고, LAN은 호스트 역할을 바꾼 연결·게임 패킷 왕복을 확인한다. 포맷 major가 다른 중간 저장을 헤더 수정만으로 허용하지 않으며, 업그레이드와 구버전으로 되돌리기의 지원 방향을 구분한다. [1.1.30 기록](releases/1.1.30.md)은 원본 소스의 설정 이관 성공 및 LAN 클라이언트 방향의 미통과를 함께 남긴다.

후속 보안 점검은 기존 NP-01/02·CJ-10·FS-01/02에 연결한다. 패킷/파일 길이·peer 권한·처리량과 큐의 상한·불량 입력 뒤 복구를 생성 입력으로 확인한다. 통신 지연을 줄이기 위해 검증을 제거하지 않으며, 정상 입력의 처리 비용과 오류 입력의 자원 사용을 별도로 측정한다. 이 문단은 점검 계획이며 보안 감사 완료나 침투 시험 성공을 뜻하지 않는다.


FS-05는 `save-manager-allocation-capture|save-manager-allocation-publish`, `cart-replacement-capture-*`, `frontend-close-capture*`로 확인한다. 호출 스레드의 지정 크기 `new[]`만 실패시켜 최초/성장 버퍼와 보조 버퍼의 실패 전달·기존 파일 보존·최신 데이터 미확보 시 성공 방지·다음 부분 쓰기에서 전체 복구를 검사한다. 실제 코어의 DS/GBA 저장·생성 AP 설정·합성 외부 firmware를 새 게임 쓰기 없이 재확보한다. 종료 창은 실제 추출 메서드·Qt 대화상자와 producer/save 대역을 사용해 일시정지·Retry·SaveCopy 순서를 확인한다. 별도 생성 DS/DSi/GBA 실행은 전체 Qt EmuThread·카트·Platform callback에서 실패 후 다음 프레임 복구를 확인한다. 통제된 할당 실패를 자연 메모리 고갈·실물 chip·장기 경쟁 수락으로 확대하지 않는다.

1.1.72의 `cart-replacement-ds-capacity-`4그룹은 원본 길이·내용을 기준으로 실제 로더/칩 SPI와 SaveManager의 첫 쓰기·후속 wrap/full-chip 쓰기·재로드를 검사한다. 등록 메타데이터와 큰 파일의 패딩, 미지원8MiB 파일의 SPI/NAND 구분, 서로 다른 물리 용량의 상태 복원·프로토콜·후속 section·거부 입력도 확인한다. 현재 writer의 헤더를14.1로 바꾼 검사는 **합성 legacy-layout 검사**이며 구버전 writer 실행으로 세지 않는다. 원본1.1의 실제 게임 상태600프레임 검증은 별도다. 파일 여분은 상태에 직렬화하지 않으므로 상태만으로 해당 여분을 이관/백업한다고 보장하지 않는다.

1.1.73의 DS 수동 선택9그룹은 `cart-replacement-ds-manual-`·`asset-ui-ds-save-`·`rom-preparation-ds-save-`·`ds-save-request-consumer`다. 실제 로더/SPI/SaveManager와 추출한 Qt 선택/큐 소비 검사를 구분한다. 별도 실제 앱6그룹은 QAction→파일/종류 선택→준비→실제 소비를 거치고, 첫 쓰기/재로드·파일 여분·취소/재선택·일반/최근/드롭 기본값을 확인한다. 생성 입력과 offscreen/dummy 장치를 사용하며 물리 장치나 모든 chip 명령의 검증으로 세지 않는다. 전체866/867의 파일 교체 실패와 단독1회 통과, JIT OFF20/21의 잘못된 R4 태그 기대값 및 실제 클래스 검사로 수정한1/1 결과를 각각 보존한다. 같은 성공 검사를 다시 실행해 최초 실패를 지우지 않는다.

1.1.74의 `retail-flash-`4그룹은3용량·페이지 끝/초과·읽기·CS 해제·지우기 주소 정렬/거부·대기 상태와 손상 입력을 확인한다. `cart-replacement-ds-capacity-flash`는 실제 SaveManager의 선행 캡처 뒤 쓰기·지우기·파일 여분·재로드, `cart-spi-flash-state`는 실제 MMIO/scheduler와 DS/생성 DSi·ARM9/7·IR의13경로를 확인한다. 후자는 CTest 출력 상한으로 성공 로그가 잘리므로 검토한 테스트 분기와 프로세스 성공을 근거로 삼는다. 별도1.1.73 소스의 writer/reader 대조는 헤더를 수정하지 않은 실제 구형 프로그램 실행이며, 원본1.1 게임 상태 재생과 구분한다. 초기 Flash4실패, 통합 IR2실패의 원인/수정, 전체872/873 및 기존 Qt10 거부의 단독1회 통과를 보존한다. 물리 chip timing/WIP/WP·전체 콘솔 실패 원자성의 검증은 아니다.
