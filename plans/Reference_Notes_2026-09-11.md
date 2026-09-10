# 추가 개발 참고 자료와 다음 확인

2026-09-11에 저장·통신·JIT·그래픽 네 영역을 독립 수집하고 원문을 대조해 선별했다. 아래 자료는 구현 계약과 검증 방법의 참고이며, melonDS의 새 결함 재현·성능 향상·실기 일치 판정이 아니다. 실제 실행 순서는 [후속 블록](Execution_Order.md#다음-독립-블록)을 따른다. `master` 링크는 조회 시점의 자료이며 채택 시 해당 소스 revision을 고정한다.

| 자료·확인 위치 | 확인한 내용 | 연결할 과제·다음 확인 |
|---|---|---|
| [Qt QSaveFile](https://doc.qt.io/qt-6/qsavefile.html), 개요·commit·cancelWriting·setDirectWriteFallback | 임시 파일에 쓰고 성공 시 최종 이름으로 교체한다. 쓰기 실패를 기억하며 commit 실패/취소는 임시 파일을 폐기한다. direct-write fallback은 원본 보존 보장이 없다. | FS-02/06: FAT export 두 번째 블록에서 실패해도 기존 목적지와 index를 보존하는 계약. Qt는 frontend 기능이므로 core에 Qt 의존성을 넣기 전에 현재 Platform 경계에서 이용할 방법을 검토한다. |
| [Dolphin IOFile](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Common/IOFile.h), ReadArray·WriteArray·IsGood | 요청한 개수와 실제 read/write 결과를 비교하고 실패 상태를 누적한다. | FS-02/06: FAT/NAND export의 짧은 I/O를 성공으로 처리하지 않는 모델 참고. stdio wrapper를 그대로 추가하기보다 현재 Platform 반환값과 오류 전달을 먼저 사용한다. |
| [SQLite Atomic Commit](https://www.sqlite.org/atomiccommit.html), 3.7~3.11 | rollback journal과 실제 데이터의 flush 순서를 구분하고 마지막 commit 지점을 설명한다. 파일시스템·저장장치 가정도 명시한다. | FS-06: 함수 성공, 파일 교체, 전원 장애 내구성을 별도 판정한다. SQLite 도입이나 FAT 전체 transaction 추가는 채택하지 않았다. |
| [ENet 1.3.18 header](https://github.com/lsalzman/enet/blob/5a9c537fd464b3c6d3c55e1d3bd47588faf71b42/include/enet/enet.h), ENetEvent·ENetHost·packet flags | event data는 32비트이며 수신 packet은 사용 후 해제하는 계약이다. reliable 전송과 peer의 최대 packet/대기 데이터 한도가 구분된다. | NP-01/02/03: 기존 v1 선택 확장과 peer/packet 소유권·대기량 한도 대조. ENet 한도는 우리 LocalMP 목적지 버퍼 용량을 대신하지 않는다. |
| [Dolphin NetPlayProto](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/NetPlayProto.h), ConnectionError·채널 enum | 연결 오류를 별도 코드로 알리고 일반 전송과 대용량 전송 채널을 구분한다. | NP-01/02: 오류 전달·채널 소유권 참고. melonDS의 원본 v1 호환과 지원 상대만의 확장이라는 현재 계약을 유지한다. |
| [GDB qCRC 계약](https://sourceware.org/gdb/current/onlinedocs/gdb.html/General-Query-Packets.html), [GNU libiberty xcrc32](https://github.com/gcc-mirror/gcc/blob/master/libiberty/crc32.c) | 메모리 CRC는 MSB 우선, 초기값 `FFFFFFFF`, 다항식 `04C11DB7`, 최종 반전 없음이다. 일반 파일의 reflected CRC32와 다르다. | NP-10/11: 길이·주소 wrap 거부와 checksum 의미를 별도로 확인한다. 공식 구현을 독립 실행한 ASCII `123456789`의 기준값은 `0376E6E7`이며, 빈 입력과 127/128/129바이트를 실제 명령 handler와 대조한다. ROM·저장용 CRC 함수까지 바꾸지 않는다. |
| [QEMU Translator Internals](https://www.qemu.org/docs/master/devel/tcg.html), CPU state·block chaining·SMC·exception | 번역 블록의 상태 키, 인터럽트가 열릴 수 있는 상태 변경 뒤 main loop 복귀, 코드 무효화·체인 해제와 host PC→guest PC 복원을 설명한다. | CJ-16/18: 이미 컴파일된 블록의 상태 변경·예외·무효화 대조. 문서의 구조 설명은 XXH3 비용이나 DS 사이클의 성능/정확성 근거가 아니다. |
| [Dynarmic A32 fuzz_arm](https://github.com/azahar-emu/dynarmic/blob/master/tests/A32/fuzz_arm.cpp), 명령 생성·RunTestInstance | 무작위 A32/Thumb 블록을 만들고 JIT와 Unicorn을 함께 실행하는 차등 검증 구조다. 후속 azahar-emu 포크 기준이다. | CJ-06/16: 기존 guest fixture에 값·플래그·메모리 비교를 추가할 때 참고. ARMv7 계열 비교기의 제외 목록·상태 mask를 DS ARMv4T/v5TE 기대값으로 복제하지 않는다. |
| [Testing CPU Emulators](https://rpaleari.github.io/pubs/issta09.pdf), Martignoni 외, ISSTA 2009, 방법론 | 생성한 프로그램을 에뮬레이터와 CPU에서 실행해 관찰 가능한 아키텍처 상태를 비교하는 방법을 다룬다. | CJ-06/16: 의미 차등 검증 참고. IA-32 대상 연구이므로 ARM/DS 타이밍 기대값은 별도다. |
| [Automatically Locating ARM Instructions Deviation between Real Devices and CPU Emulators](https://arxiv.org/abs/2105.14273v2), Jiang 외, 2021, 초록·방법론 | ASL 사양 기반 명령 생성과 ARM 실기/QEMU 차등 실행을 제시한다. 명세의 정의 범위와 에뮬레이터 구현 오류를 구분한다. | CJ-06/16: 합법 입력·미정의/구현 의존 입력을 나누는 방법 참고. DS의 ARM7/9 메모리 맵·사이클이나 모든 비정렬 동작의 oracle은 아니다. |
| [Khronos glDispatchComputeIndirect](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glDispatchComputeIndirect.xhtml), [공식 XML](https://github.com/KhronosGroup/OpenGL-Refpages/blob/main/gl4/glDispatchComputeIndirect.xml), Description·Errors | 간접 명령의 work-group 수가 한도를 넘으면 일반 dispatch와 달리 오류가 보장되지 않으며 동작이 정의되지 않는다. offset/명령 버퍼 범위 오류와 구별한다. | GR-07: 실제 producer가 만든 indirect 수치와 지원 한도를 실행 전에 대조한다. 잘못된 명령을 실제 GPU에 보내 오류가 나는지 보는 방식 대신 사전 거부·안전한 분할을 검증한다. |
| [Khronos glMemoryBarrier](https://registry.khronos.org/OpenGL-Refpages/gl4/html/glMemoryBarrier.xhtml), [공식 XML](https://github.com/KhronosGroup/OpenGL-Refpages/blob/main/gl4/glMemoryBarrier.xml), barrier bits | shader buffer·texture·command 등 이후 접근 종류에 따라 가시성 계약이 다르다. | GR-06/07/08: producer→소비자별 barrier를 대조한다. host 동기화와 guest capture scanline 시각도 별개로 검증한다. |
| [parallel-rdp rdp_renderer.cpp](https://github.com/Themaister/parallel-rdp/blob/1cecd042b2619bc505c12bfdc713808386f2b54d/parallel-rdp/rdp_renderer.cpp), 용량 산정·need_flush·flush_queues | 배율에 따른 tile 용량을 산정하고 cache/span/tile 작업량으로 flush 여부를 정하는 Vulkan compute 구현이다. | GR-07/08/14: 작업량을 나눠 처리하는 후보의 구체 구조 참고. N64/Vulkan의 래스터 규칙을 DS/OpenGL에 그대로 적용하지 않으며 출력 보존과 처리 비용을 따로 측정한다. |

## 수집에서 구체화한 후속 범위

- **FAT에 이어 NAND export도 같은 실패 주입을 검토한다.** 현행 두 ExportFile은 `f_read`와 host write 결과를 대조하지 않는 경로가 있다. 기존 목적지를 여는 `FileMode::Write`는 truncate 계약이므로, 단순히 앞부분만 바뀌는 것으로 가정하지 않는다. 두 번째 블록의 짧은 읽기/쓰기와 실패 반환·목적지 보존을 각각 재현한다. NAND 이미지 자체 transaction과 host export는 구분한다.
- **GPU 간접 실행은 사전 한도 검증을 별도 항목으로 둔다.** 수집 초안의 일반 dispatch 오류 규칙을 간접 dispatch에도 적용한다는 해석은 원문 대조에서 제외했다. 필요한 수정은 한도 조회와 실제 생산 값의 관계를 확인한 뒤 결정한다.
- **ISA 차등 검증의 비교기 한계를 기록한다.** 다른 에뮬레이터 둘이 같은 값을 내도 DS 실기의 값·사이클을 증명하지 않는다. 미정의 인코딩을 임의로 제외해 기존 반례가 사라지게 하지 않는다.

이 자료 수집으로 새 엔진·라이브러리·프로토콜 전면 교체를 결정하지 않았다. 이미 해결된 과제는 다시 구현하지 않고, 다음 반례의 입력·성공 조건·남은 외부 수락을 기존 ID에 붙인다.
