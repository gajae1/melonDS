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
최종 구성의 전체 검사는 필요할 때 한 번 실행하고 범위가 같은 성공 결과를 재사용한다.

```sh
ctest --test-dir build/windows-dev --no-tests=error --output-on-failure
```

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
