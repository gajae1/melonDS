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

## 진단 구성

루트 [Sanitizers.cmake](../cmake/Sanitizers.cmake)의 `SANITIZE` 설정은 도구와 runtime이 지원하는 조합에서 사용한다. 작은 parser·buffer 경계는 기존 독립 테스트를 우선한다.
JIT fastmem의 의도된 fault와 실제 메모리 오류를 구분한다. sanitizer runtime이 없거나 충돌하면 실행하지 못한 이유를 남기며 전체 검사를 꺼서 통과로 바꾸지 않는다.
ASan/UBSan/TSan의 지원 조합 표와 추가 JIT 통합은 BV-11의 후속 과제다. 이 문서 자체는 sanitizer 실행 증거가 아니다.

## 증거와 측정

한 변경 기록에 소스 기준·변경 ID·빌드 종류·compiler/의존성 버전·관련 옵션·실행 명령·입력 종류·기대 결과·실패/성공/skip·남은 범위를 적는다.
재현 입력은 합성 자료를 우선하고 실제 게임·저장 파일은 공개 자료에 넣지 않는다. 공개 가능한 digest와 필요한 비식별 환경 정보만 사용한다.

성능 결과는 같은 소스 조건·입력·출력·renderer·JIT 설정의 전후 비교로 기록한다. warm/cold, 중앙값과 분산, CPU/GPU/upload/readback/audio 대기를 구분한다.
실측하지 않은 percentile이나 성능 이득을 채우지 않는다. 커널 시간 단축을 전체 FPS·입력 지연 개선으로 환산하지 않는다.
compiler profile을 학습한 workload와 평가 workload를 분리한다. 소스·compiler가 바뀐 PGO profile은 재검증한다.

패키지는 생성된 실행 파일·필요 DLL/plugin·라이선스만 선별한다. 원본 저장 데이터는 보존한다.
독립 PATH에서 실행 파일의 `--help`와 필요한 frontend·plugin 경로를 확인한다. `--help` 성공은 실제 게임·장치 수락을 대신하지 않는다.
