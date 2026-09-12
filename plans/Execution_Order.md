# 착수 순서

먼저 실제 소스와 후보를 대조하고 실패·정상 입력 한 쌍을 준비한다. 재현되면 최소 수정하고, 반증되면 정상 구현을 유지하며 근거를 남긴다.
우선순위는 발생률이나 완료 상태가 아니다. 다음 다섯 묶음은 책임 버전을 변경하지 않는 실행 순서다.

| 순서 | ID / 책임 버전 | 첫 반례와 정상 대조 | 완료 조건·남는 범위 |
|---|---|---|---|
| 1 | CJ-09/10, FS-01 / 1.1.05 | 활성 event의 범위 밖 FuncID·미등록 callback; 앞 RAM 적용 뒤 후반 section 실패; 정상 상태·undo | event 입력 거부와 전체 로드 rollback을 별도로 검증한다. rollback 자체 실패 시 실행을 재개하지 않는다. |
| 2 | FS-08/04/02 / 1.1.07 | 출력 크기 미기록 Zstd·잘린 frame; 손상 TOML로 Load→Save; 정상 사용자 지정 설정 | 실제 출력 길이·frame 완료·오류 반환·원본 보존. ROM 교체/reset 전에 실패를 반환한다. |
| 3 | FS-03/05/06/13 / 1.1.04 | A 저장 pending 중 실패하는 B 교체; flush 중 SetPath/버퍼 교체; 종료까지 쓰기 거부 | 경로·버퍼·버전의 소유권 일치, 다른 게임에 잘못 저장하지 않음, 미저장 결과·복구 선택 전달. |
| 4 | AD-01/02, FS-09/10 / 1.1.08·06 | mic upsample 끝 sample·생산/소비 count; controller→joystick; touch cancel/focus | 메모리 경계·동기화·닫힌 handle·입력 snapshot 검증. 실제 장치 수락은 합성과 구분한다. |
| 5 | NP-01/05~07/10~12 / 1.1.12·11·10 | 짧은 LAN payload·AID0; PCap caplen<len·잘린 DNS; GDB M/X 길이·escape | 잘못된 입력에서 부분 쓰기·과잉 복사 없음. 정상 프로토콜·재접속·연결 종료 보존. |

첫 증분은 CJ-09의 이벤트 값 검증과 FS-08/FS-04의 파일 경계·설정 보존을 1.1.03에 선반영한다. CJ-10/FS-01 전체 rollback, FS-02 전체 읽기·reset 경계, 설정 UI의 상세 복구 흐름은 각각 원래 과제에서 계속 관리한다.

1.1.04 증분은 CJ-10/FS-01의 load·undo 복원과 FS-02의 상태 파일 읽기 경계, FS-05 저장 잠금을 다룬다. [구현·검증 범위](releases/1.1.04.md)를 확인한 뒤 FS-03 ROM 교체의 저장 소유권과 FS-06 종료까지 지속되는 쓰기 실패 처리를 이어간다. 카트·DSi·host queue를 포함한 전체 상태 수락은 1.1.05 책임 범위에 남긴다.

[1.1.05 증분](releases/1.1.05.md)은 FS-03 교체 소유권, FS-06 종료 복구와 FS-02 카트 SRAM 읽기의 구현·로컬 회귀를 확인했다. [1.1.06 증분](releases/1.1.06.md)은 FS-02의 ROM 원시 읽기·save import 적용 순서, FS-07 archive 오류와 FS-14 RTC 파일 보호를 검증했다. 다음에는 입력·터치·마이크 묶음을 확인하고 네트워크 묶음을 이어간다. FS-13 이름 충돌은 기존 파일명 정책을 바꾸는 별도 작업으로 유지한다.

[1.1.07 증분](releases/1.1.07.md)은 FS-09/10의 장치·터치 수명과 AD-01/02의 마이크 입력 경계를 재현하고 로컬 회귀를 확인했다. 다음은 NP-01/05~07/10~12의 LAN·PCap/Slirp·GDB 입력 묶음이다. 실제 장치·플랫폼 수락을 기다리는 항목은 유지하며 독립적으로 재현 가능한 네트워크 경계를 먼저 다룬다.

[1.1.08 증분](releases/1.1.08.md)은 NP-01/05~07/10~12의 패킷 경계·메모리 요청·연결 종료를 수정하고 생성 입력과 실제 Windows loopback으로 확인한다. 빈 AID0 reply는 정상 구현으로 보존한다. 두 PC·실제 어댑터·DS DNS·ARM debugger 수락은 남긴다. 다음 증분은 NP-16/20/21의 homebrew·치트 파일/실행 입력을 실제 source와 대조하고, 잘린 입력·정상 입력 한 쌍부터 재현한다. FS-13 파일명 충돌, LAN handshake/LocalMP 수명과 다른 미착수 과제도 계속 유지한다.

[1.1.09 증분](releases/1.1.09.md)은 NP-16/20/21의 argv/DLDI와 치트 DB·literal 입력을 검증하고, 긴 치트 실행에 UI 메시지의 취소 요청을 연결한다. 정상 코드에 대한 임의의 작은 반복 상한은 추가하지 않는다. 다음은 FS-13 저장 파일명 충돌과 NP-13~15/17~19의 주변장치·파일 형식 후보를 소스·생성 입력으로 좁힌다. NP-21의 D1/D2 조건 복원과 C5 수명은 독립 의미 검증으로 유지하며 실기 수락을 추측으로 채우지 않는다.

[1.1.10 증분](releases/1.1.10.md)은 FS-13의 명시적 파일 선택과 활성 경로, NP-13 IR 위치·미지원 응답, NP-19 RTC edge·분 carry·Reset, NP-17 진동 AD1을 구현했다. 다음은 NP-18 EEPROM page wrap/dirty 통지와 AUXSPI CS 해제의 정상/실패 대조다. NP-14 BT 프로토콜과 NP-15 형식 판별은 근거를 확보한 부분만 좁혀 진행한다. 센서·RAM·실제 무선 통신과 모든 앞선 장치 수락은 그대로 남긴다.

[1.1.11 증분](releases/1.1.11.md)은 NP-15의 AUXSPI 부분 쓰기로 인한 CS 불일치와 NP-18의 작은 EEPROM page wrap/저장 통지를 재현·수정했다. 다음은 FS-15 치트 편집의 원자적 저장·오류 전달이다. 코어의 직렬화와 Qt 파일 commit을 분리하고 정상 roundtrip·쓰기 실패·편집 유지부터 검증한다. GBA EEPROM은 실제 DS ROM bus/DMA 소비자를 통한 read/write부터, BT는 command/IRQ/reply transport부터 독립 구현한다. 현재 부분 성공으로 이 기능들을 완료 처리하지 않는다.

[1.1.12 증분](releases/1.1.12.md)은 FS-15 편집 사본·직렬화 사전 검사·Qt 파일 commit과 실패 복구·실행 목록 갱신을 연결한다. 다음은 NP-21의 D1/D2 조건 복원과 C5 수명을 생산 실행기·정상 코드·명세와 대조한다. 결함을 가정해 게스트 의미를 먼저 바꾸지 않는다. LAN/LocalMP 수명·GBA EEPROM·BT와 앞선 실제 게임·장치 수락은 남은 독립 과제로 유지한다.

[1.1.13 증분](releases/1.1.13.md)은 NP-21의 D1/D2 매 반복 조건 복원·D0 기본 상태를 재현·수정하고 C5 원저자 자료를 대조한다. NitroHax의 영속 카운터가 Datel의 수명 증거는 아니므로 Datel 원 핸들러·실기 판정은 남긴다. 다음은 CJ-03 warmed ALU·shift·부분 flags와 CJ-06 빈 LDM/STM 목록의 CPU별 정의·실행 대조다. 현재 환경에서 독립 검증 가능한 core/JIT 경계를 진행하며 LAN·GBA EEPROM·BT와 장치 수락을 완료 처리하지 않는다.

[1.1.14 증분](releases/1.1.14.md)은 입력 설정의 실제 사용 문제를 우선 처리한다. SDL 장치가 있으면 Joystick 화면을 자동으로 여는 동작을 제거하고 Keyboard·Joystick 탭, 실제 dialog 저장·실행 적용·새 프로세스 재로드를 검증한다. 이어 CJ-03/CJ-06을 진행한다. 책임 버전 1.1.14의 JIT 과제는 미착수 상태이며 UI 수정으로 완료 처리하지 않는다.

[1.1.15 증분](releases/1.1.15.md)은 CJ-03의 실제 warmed ALU·shift·부분 flags를 보강하고 x64 ASR의 잘못된 호스트 피연산자와 Thumb 내부 사이클 누락을 재현·수정한다. 두 게임에서 일반 JIT/fastmem과 이전 코어 JIT의 최종 화면 digest는 일치한다. 인터프리터와의 기존 차이는 원인 미확정으로 유지한다. 다음은 CJ-06의 근거가 있는 ARM7 빈 목록 전송·PC·writeback부터 interpreter/decode/기존 fallback을 함께 연결한다. ARM9 호환 모델과 빈 PUSH/POP·DS 실기 타이밍을 ARM7 근거만으로 확정하지 않는다. CJ-03의 PC 복원·조건과 native A64도 남긴다.

[1.1.16 증분](releases/1.1.16.md)은 CJ-06의 ARM/Thumb LDM/STM 빈 목록을 CPU별로 처리하고, CJ-03의 일반 ARM7 STM PC값과 상태 전환 후 동일한 pipeline PC의 JIT 분기 누락을 수정한다. 빈 PUSH/POP의 원 실기 oracle은 확보하지 못했으므로 보류하며 기존 LR/PC 단독 경로와 구분한다. 다음은 CJ-05 DTCM remap 구간과 CJ-07 이벤트 취소의 실제 반례다. CJ-04 MPU·전체 CPU 명령·앞선 장치 수락도 유지하며, 원래 책임 버전과 실제 증분 릴리스 번호를 혼동하지 않는다.

[1.1.17 증분](releases/1.1.17.md)은 CJ-05의 DTCM 자체 view·이전 RAM 제외 구간·작은 bank offset과 CJ-07의 취소된 due event 실행을 수정한다. Windows 실제 view와 warmed guest, 재예약·snapshot 계약을 검증했다. 다음은 CJ-04의 MPU 권한 갱신과 exception 진입을 실제 guest의 정상/abort 한 쌍으로 좁힌다. CJ-08의 IRQ/DMA/sleep은 독립 이벤트 경계부터 확인하며, resize/save-load·다른 OS·native A64·장기 게임과 앞선 미완료 과제는 유지한다.

## 실제 증분 1.1.18 — 코어 경계와 오디오 진단 병행

CJ-04/CJ-08의 기존 착수 범위에 AD-04의 간헐적 틱 노이즈와 AD-08의 보간 품질 비교를 함께 배치한다. 보간을 켜고 출력 버퍼를 낮추면 드물게 발생한다는 사용 보고이며, 특정 보간 종류·버퍼 값·장치와 원인은 아직 확정하지 않았다. 아래 실제 증분 번호는 Release_Plan의 DSi 책임 버전 번호와 구분한다.

1. **재현·계측:** 같은 오디오 구간에서 보간 끔/켬과 낮은 버퍼/기본 버퍼를 대조한다. callback의 요구/공급 샘플 수, underrun·drop·대기와 출력 파형의 불연속을 계측한다. 평균 FPS나 CPU 사양만으로 원인을 판단하지 않는다.
2. **원인 수정:** 확인된 공급·동기화·무음 전환/재개 경계를 고친다. 기본 버퍼 확대나 저역통과 필터만으로 증상을 숨긴 것을 해결로 집계하지 않는다.
3. **보간 비교:** 기존 채널별 보간과 대역 제한 sinc 후보를 같은 입력에서 비교한다. 이미 있는 최종 출력 blip_buf 변환과 구분하고 음질·alias 억제·연산량·추가 지연을 평가한다. 개선 근거를 확보한 경우만 선택 옵션으로 통합한다.
4. **검증:** 기존 오디오 회귀와 재현 구간의 반복 실행으로 잡음·버퍼·지연을 대조한다. 생성 PCM 검사와 실제 장치 청취를 구분하고, 보간/버퍼 설정 저장·재시작 적용을 확인한다.
5. **출시 판정:** 입증한 수정과 비교 결과만 배포 기록에 반영한다. 드문 잡음의 미재현을 해결로 선언하지 않으며, sinc 채택 여부와 실제 장치·장기 실행의 남은 범위를 명시한다.

[1.1.18 구현 기록](releases/1.1.18.md)은 CJ-04의 warmed 정적 분기 권한 취소, AD-04의 생산량 기준 동기화와 부족/복귀 처리를 다룬다. AD-08 Q3는 고정 timer에서만 비교했고 제품 채택을 보류한다. 전체 MPU·장기 청취·물리 지연을 완료 처리하지 않는다.

## 실제 증분 1.1.19 — 코어 장치 이벤트·Compute 경계

CJ-08과 GR-02/03을 병행한다. 오디오 후보 확대에 머물지 않고 실제 코어·영상 소비자의 정확성과 비용을 계속 검증한다.

1. **근거·재현:** CJ-08은 IRQ/DMA/sleep 중 한 동시 경계를 생성 guest·장치 trace로 고른다. GR-02/03은 현재 Compute capture sampler와 capability/shader 실패 전달의 실제 반례·정상 대조를 준비한다.
2. **최소 수정:** 실행으로 확인된 예외 순서 또는 sampler/오류 전달만 수정한다. 예측한 결함이나 새로운 renderer 전체를 함께 넣지 않는다.
3. **통합:** 기존 guest 시간 단위·renderer fallback과 설정 저장 경로를 유지한다. 입력·오디오 등 공유 frontend 파일의 작성자는 하나로 둔다.
4. **검증:** 기존 core/GPU 회귀와 실제 OpenGL/Compute 출력·readback을 사용한다. 게스트 결과가 바뀌면 같은 게임·이전 코어와 대조하고 CPU 처리·GPU 대기를 구분한다.
5. **배포:** 입증한 범위만 다음 실행 파일과 공개 계획에 반영한다. 다른 OS/native A64·실기 타이밍·장기 플레이와 앞선 미완료 항목은 유지한다.

[1.1.19 구현 기록](releases/1.1.19.md)은 CJ-08의 Timer0→HALT→IRQ F 보존, GR-02의 capture sampler unit, GR-03의 capability 판정·실제 shader 실패·프런트엔드 복구를 검증한다. Windows 397개 검사와 두 게임의 10개 현재 최종 프레임은 통과했으며 같은 조건의 1.1.18 결과와 일치한다. 실제 GL3.2-only 장치·설정창 표시·전체 Qt event loop와 DMA/sleep·실기 타이밍은 남긴다.

## 실제 증분 1.1.20 — 캡처 보존·GL 자원 수명

GR-01/04/06을 진행한다. 같은 backend의 배율 변경 전후 데이터와 실제 source A/혼합 캡처 소비자를 확인한다.

1. **근거·재현:** GPU에만 남은 캡처→1x/2x 변경→CPU 읽기의 정상/실패 대조를 만든다. 별도 작업자는 실제 GL 생성·삭제를 계수해 정상 teardown과 첫 shader 실패의 자원 수명을 확인한다.
2. **최소 수정:** 재할당 전 필요한 캡처를 보존하고 확인된 누락 해제·미초기화 ID만 수정한다. 추정한 캡처 중간 타이밍·전체 RAII 재설계는 함께 넣지 않는다.
3. **통합:** source A/B·혼합 캡처의 metadata, 기존 texture cache·CPU/DMA 가시성·배율 설정 경계를 연결한다. 같은 파일과 공유 빌드는 한 작성자가 통합한다.
4. **검증:** 실제 두 GL backend·software와 CPU 읽기를 비교하고 재현된 크기·wrap·배율 조합을 검사한다. resource는 현재 context를 유지한 채 소유 객체의 소멸과 무관한 객체의 생존을 확인한다.
5. **배포:** 검증한 범위만 Windows 실행 파일·공개 기록에 반영한다. 중간 캡처 시각·실기·다른 드라이버/OS·native A64와 앞선 미완료 과제는 남긴다.

[1.1.20 구현 기록](releases/1.1.20.md)은 배율 변경의 캡처 유실·이미 읽은 캡처의 texture 참조, 반복 teardown의 program/buffer 누락과 초기 실패의 오삭제를 수정한다. source A/B·혼합·wrap·실제 guest LDRH·후속 texture 재사용을 검사했다. Windows 403개 검사와 두 게임의 세 renderer별 6개 현재 최종 프레임은 통과했으며 같은 조건의 1.1.19 프레임과 바이트가 일치한다. 중간 색 강도의 기존 1단계 차이는 GR-08 oracle 비교로 분리하고 DMA·캡처 진행 중 시각을 완료 처리하지 않는다.

## 실제 증분 1.1.21 — GL 한도·할당 실패와 context 경계

GR-07을 우선하고 GR-11/12의 실제 호출·소유 경계를 독립 조사한다. GR-05/08의 실기 시각·정밀도는 확인된 근거 범위에서만 진행한다.

1. **근거·재현:** 지원 배율의 크기 계산과 실제 GL 한도를 대조한다. 작은 정상 배율과 한도 초과/할당 실패 입력을 구성한다. context 생성·현재화·파괴가 어느 thread에서 가능한지 실제 호출부터 확인한다.
2. **최소 수정:** 입증한 실패에서 잘못된 자원 사용을 막고 기존 유효 출력 또는 software로 복구한다. GR-12 동시 생성이 도달 불가능하면 불필요한 global lock을 추가하지 않는다.
3. **통합:** 선택 배율·실제 활성 renderer·오류 전달·재선택의 상태를 맞춘다. 캡처 보존·부분 자원 해제·소유 thread 경계를 유지한다.
4. **검증:** 한도 판정/오류 주입과 실제 driver 할당 실패를 구분한다. 정상 픽셀·복구 뒤 프레임·재시도를 확인하고 변경한 경계의 기존 GPU/프런트엔드 회귀를 실행한다.
5. **배포:** 확인한 수정만 다음 Windows 실행 파일에 반영한다. 작은 VRAM 실장치·surface 상실/휴면/다중 창의 실제 화면·다른 OS/driver와 앞선 미완료 범위는 남긴다.

[1.1.21 구현 기록](releases/1.1.21.md)은 GR-07의 배율·장치 한도·할당 오류를 설정 실패로 전달하고 software 전환·캡처 보존·재선택을 검증한다. GR-11의 borrow 반환 신호 유실은 같은 mutex의 상태값으로 수정했다. 전체 Windows 417개 검사와 두 게임의 6개 현재 최종 프레임은 통과했으며 같은 조건의 1.1.20 프레임과 바이트가 일치한다. GR-12의 creator끼리 동시 진입 후보는 현재 GUI 경로에서 반증됐지만 GLAD 재로딩과 다른 worker 호출의 중첩은 후속으로 유지한다.

## 실제 증분 1.1.22 — 표시 context의 정지·재개·실패 전달

GR-11/12에서 확인한 실제 호출 경계를 진행한다. 창 교체/해제와 전역 GLAD 재로딩이 기존 worker의 GL 호출과 겹치는 조건을 우선한다.

1. **근거·재현:** deinit 응답 뒤 GUI panel 교체 전에 paused 표시를 진행시켜 재접근을 확인한다. 새 인스턴스 생성 중 기존 worker의 GL 호출·loader 쓰기를 독립 관찰한다. 화면용 shader/MakeCurrent 실패도 코어 shader와 구분한다.
2. **최소 수정:** panel/context 접근 정지를 GUI 교체·파괴가 끝날 때까지 유지한다. 초기화되지 않은 표시 경로와 실패한 current 상태로 진입하지 않도록 한다. creator끼리만 잠그는 것으로 worker 동시 읽기 문제를 해결했다고 하지 않는다.
3. **통합:** 검증한 borrow 반환 계약과 기존 메시지 순서를 재사용한다. 정지 중 worker init 응답을 기다리는 교착을 피하고 정상 재개·renderer/표시 fallback·오류 통지를 연결한다.
4. **검증:** 두 thread의 결정적인 순서와 실제 Windows context를 가능한 범위에서 대조한다. 표시 중단·정상 재개·반복 교체·새 인스턴스의 GL 호출 경계를 검사하고 게임 프레임·입력 상태 보존을 확인한다.
5. **배포:** 확인한 경로만 다음 Windows 실행 파일과 공개 기록에 반영한다. 물리 OOM·휴면/surface 상실·다중 GPU/DPI·다른 OS와 앞선 GPU 정밀도·오디오·실기 수락은 별도 gate로 유지한다.

[1.1.22 구현 기록](releases/1.1.22.md)은 해제 후 paused draw, root 코어 renderer/capture 정리와 GUI 창 변경 중 worker 접근을 다룬다. 새 context loader 경계와 borrow 중 worker broadcast도 함께 검증했다. 표시 shader/current/swap 실패의 복구는 독립 경계로 계속 진행하며 이번 성공으로 닫지 않는다.

## 실제 증분 1.1.23 — 표시 초기화 오류와 재시도

1. **재현:** 실제 ScreenShader/OSDShader compile 실패·MakeCurrent 실패·부분 생성 상태를 정상 초기화와 대조한다. OSD의 rendered 상태와 texture 소유 수명을 재초기화 전후로 확인한다.
2. **수정:** 표시 초기화의 성공 여부와 부분 해제를 연결한다. 실패한 current 상태에서 GL 호출을 계속하지 않고 성공한 초기화만 ready로 공개한다.
3. **통합:** worker→GUI 오류 통지와 기존 native 표시·설정·재선택을 연결한다. core renderer가 context보다 오래 살아남지 않도록 하며 borrowed worker의 응답을 기다리는 교착을 피한다.
4. **검증:** 정상·실패·재시도·반복 해제와 OSD가 있는 재초기화를 실제 Windows GL에서 검사한다. 가능한 전체 Qt 경로와 paused 게임 이미지·입력 유지도 별도로 확인한다.
5. **배포:** 입증한 복구만 공개 기록과 다음 실행 파일에 반영한다. 실제 surface/장치 상실·이종 GPU·다른 OS와 앞선 미완료 과제는 유지한다.

[1.1.23 구현 기록](releases/1.1.23.md)은 표시 shader·첫 MakeCurrent 실패, 부분 해제·OSD 재생성과 전체 Qt 두 창의 native 전환·명시적 재선택을 검증한다. 이미 실행 중인 core/표시 context의 실패는 다음 증분에 남긴다.

## 실제 증분 1.1.24 — 실행 중 current/swap 실패와 해제 거부

1. **재현:** 정상 GL 게임과 paused 표시에서 root/보조 창의 MakeCurrent·SwapBuffers 실패를 주입한다. core renderer가 남은 상태의 deinit·창 교체·종료도 대조한다.
2. **수정:** 확보하지 못한 context로 frame·shader·capture·삭제를 진행하지 않는다. 성공한 해제만 GUI의 context 파괴와 교체를 허용한다.
3. **통합:** worker→GUI 오류와 명시적 재시도를 연결한다. context 재확보 후 capture/core 객체를 정리해 native로 전환하며, 지속 실패 중 게임 상태와 기존 자원을 보존한다.
4. **검증:** 실제 GL과 전체 Qt에서 일시/지속 실패·재시도·창 닫기·보조 창을 확인한다. paused 게임 이미지·입력 유지도 검증하며 물리 surface 상실과 주입을 구분한다.
5. **배포:** 입증한 실패 복구만 반영한다. 이종 GPU/DPI·다른 OS·실기·장기 수락과 공개 전체 계획의 미완료 범위는 유지한다.

[1.1.24 구현 기록](releases/1.1.24.md)은 실행 중 표시 실패 전달·core 소비 차단·해제 실패 시 창 보존과 재시도를 다룬다. 전체 Qt의 생성 DS 실행에서 주 창·보조 창의 실패·재시도, 일시정지 화면과 프레임 보내기·입력 보존을 확인했다. 물리 surface 상실이나 일반 context 반환 실패까지 완료 처리하지 않는다.

## 실제 증분 1.1.25 — context 반환과 worker 소유권

1. **재현:** DoneCurrent 실패를 일반 worker borrow와 native context 생성 경계에서 정상 반환과 대조한다. 실패를 무시한 GUI 접근·loader 재진입이 실제 가능한지 확인한다.
2. **수정:** 반환하지 못한 context의 소유권을 다른 thread에 넘기지 않는다. 실패한 borrow를 성공으로 응답하지 않으며 기존 창·core 상태를 보존한다.
3. **통합:** 기존 실패 상태·GUI 재시도와 연결하고, 거부·중첩 borrow·종료에서 mutex와 응답 수를 일치시킨다. 반환 실패가 입증되지 않은 경로는 불필요하게 재설계하지 않는다.
4. **검증:** 결정적인 worker 순서와 실제 Windows context를 이용해 실패·재시도·정상 생성·해제를 확인한다. 도달 가능한 다중 인스턴스 경계를 별도로 검증한다.
5. **배포:** 재현·해결된 경계만 반영한다. 물리 장치·다른 OS·실기·장기 검증과 공개 전체 계획의 미완료 범위를 유지한다.

[1.1.25 구현 기록](releases/1.1.25.md)은 worker 반환 실패의 거부·부분/중첩 소유권 반환과 새 context의 GUI 정리를 다룬다. 전체 Qt에서 생성 DS 두 개·세 인스턴스·네 창의 실패·재시도·native 전환을 확인했고, 생성 fallback 설정 불일치와 종료 후 앱 알림 충돌도 수정했다. 물리 driver 오류와 다른 플랫폼까지 완료 처리하지 않는다.

## 실제 증분 1.1.26 — 독립 패치 묶음

| 블록 | 구현·소유 경계 | 확인한 결과 |
|---|---|---|
| DSi 부팅: AD-09/10/13 | DSi·NAND metadata·NDS direct boot·EmuInstance | 독립 암호 vector·정확한 읽기·사전 실패의 세션 보존·늦은 실패의 정지 |
| SD 전송: AD-11/12 | DSi_SD·FATStorage | 길이/sector 경계·backing 실패·FIFO/IRQ·명시적 복구 |
| LocalMP: NP-03 | LocalMP·전용 큐 fixture | 느린 수신자 넘침·record/permit 정합·종료/새 host |
| 그래픽 설정·통합: GR-03 | OpenGLSupport·Compute·EmuThread·VideoSettingsDialog·공유 CMake | 선택/실제 renderer·worker capability·실패/Cancel/재시도와 부팅 실패의 실행 차단 |

[1.1.26 구현 기록](releases/1.1.26.md)에 반례·5페이즈·최종 빌드와 498/498 회귀·전체 Qt의 확인 범위를 기록했다. 세 구현 블록은 격리된 작업 공간에서 진행했고 공유 연결부는 순차 통합했다. 새 제품 소스 파일은 없다. 실제 게임·물리 장치·장기 수락과 전체 계획의 미착수 과제는 유지한다.

## 실제 증분 1.1.27 — 렌더 대기·NDMA 정지·Wi-Fi 응답

| 블록 | 구현·소유 경계 | 확인한 결과 |
|---|---|---|
| Software 3D: GR-13 | GPU3D_Soft·GPU reset·software thread fixture | 작업 시작 전 저장/재설정 경쟁·완료 이중 대기·abort/중단·재시작·실제 픽셀/상태 복원 |
| DSi NDMA: AD-16 | DSi GX stall·DSi_NDMA·기존 core device fixture | 포화 FIFO 뒤 명령·완료 IRQ 보존·경쟁 채널·정상 즉시/VBlank 전송·저장/복원 |
| DSi Wi-Fi: NP-08 | DSi_NWifi·SDIO/WMI fixture | 명령 거부·기존 연결/scan 보존·association 본문·IRQ/credit 순서·명시적 재연결 |

[1.1.27 구현 기록](releases/1.1.27.md)에 수정 전 반례와 최종 Windows 빌드·509/509 회귀·전체 Qt의 renderer 교체/재시도/종료를 기록했다. NDMA와 Wi-Fi는 격리된 작성자가 맡고 공유 연결부는 순차 통합했다. 새 제품 소스 파일은 없다. AD-15의 장치 완료 타이밍은 변경하지 않았으며 각 항목의 물리·실제 게임·장기 수락은 유지한다.

## 실제 증분 1.1.28 — 캡처 중간 접근·태그·재설정

| 블록 | 구현·소유 경계 | 확인한 결과 |
|---|---|---|
| 캡처: GR-05 | GPU_OpenGL·기존 GL/core capture fixture | 중간 CPU 읽기/쓰기·DMA·과거/미래 줄·source/목적지/크기 변경 |
| AES: AD-14 | DSi_AES·단일 real-core fixture | FIFO/레지스터 태그·짧은 tag/padding·독립 암호 vector·FIFO/NDMA·복원 |
| DSi reset/I2C: AD-17/22 | DSi·DSi_I2C·기존 CoreDeviceExecution | SCFG와 실제 클럭/카드 경로·방향/STOP/IRQ·이전 상태 호환 |

[1.1.28 구현 기록](releases/1.1.28.md)에 수정 전 반례와 최종 Windows 빌드·517/517 회귀·전체 Qt 검증 범위를 기록했다. 기존 GPU·DMA·I2C·저장 도구를 재사용하며 새 제품 소스 파일은 없다. 실제 게임·물리 타이밍·부팅·NWRAM 등 미확정 조건은 완료로 보지 않는다.

## 실제 증분 1.1.29 — LAN·입력 identity·타이틀 교체

| 블록 | 구현·소유 경계 | 확인한 결과 |
|---|---|---|
| LAN: NP-02/FS-17 | LAN·MPInterface·LANDialog와 공유 frontend 호출 | 빈 제어 packet·초기화/종료·참가자 목록·비동기 연결/취소·실제 loopback/Qt 재시도와 수명 |
| 입력: FS-12 | EmuInstanceInput·JoystickSelection·InputConfig·Config | 누락/재정렬/동일 모델·숫자 설정 이관·인스턴스 배정·Cancel·설정 재로드 |
| DSi title: FS-16 | DSi_NAND·TitleManagerDialog | 입력/TMD 확인·기존 save 보존·NAND staging/backup·rollback·복구/정리 실패 경고 |

[1.1.29 구현 기록](releases/1.1.29.md)의 최종 Windows 빌드와 548/548 회귀·기존 전체 Qt 생성 guest 검증은 통과했다. 실제 매체·두 PC 게임/다자간 mesh·물리 hotplug·다른 OS 수락과 공개 전체 계획은 유지한다. 고유 serial 없는 패드는 재연결 뒤 재선택하며, NAND transaction은 전원 상실 복구를 보장하지 않는다.

## 다음 독립 블록

1.1.30은 실제 다자간 LAN 반례, 성공 load/undo의 host 오디오 이력, source A→DMA-first/warmed JIT의 기존 검증 공백을 세 블록으로 작성했다. LAN v1 호환·지원 상대끼리의 포트 교환·늦은 peer 준비 통지, 성공 복원과 실패 복귀의 PCM 정책을 구현했고 캡처는 제품 코드 변경 없이 생성 장면 검증을 보완했다. 원본/구버전 설정 이관과 LAN 역할 교환의 성공·실패를 구분한다. 개별 결과와 최종 통합 판정은 [릴리스 기록](releases/1.1.30.md)을 따른다.

[1.1.31 증분](releases/1.1.31.md)은 기존 FAT 이미지의 마운트 실패를 포맷으로 처리하던 경로, BTDMP의 한-word 빈 큐 접근, Compute layer·variant 초기화를 다룬다. FAT은 실제 생성 이미지/인덱스/폴더 보존과 같은 이미지 재시도를, DSP는 checked STL 반례와 실제 I2S 호출 경로를 확인한다. 부족 오른쪽 0은 기존 Teakra의 안전정책으로 명시하며 실기 정확성으로 간주하지 않는다. Compute는 전후 생성 출력이 같았고 관찰된 화면 오류나 속도 향상으로 기록하지 않는다.

[1차 감사](Audit_2026-09-11.md)의 다음 후보는 malformed state의 section 경계, FAT export 중간 실패·ROM/GBA/firmware 길이, GDB·LocalMP의 수신 경계, GL alpha/capture 및 Compute workload 한도다. 먼저 호출 가능한 최소 반례를 만든 뒤 필요한 소유 파일만 수정한다. 동일 소스 현상은 기존 ID에 합치며, 실기 값이 없는 임의 zero-fill·작업 생략·사이클 변경을 정확한 기본값으로 채택하지 않는다. 후속 네트워크 보안 점검은 NP-01/02/03/07의 패킷 길이·송신자/peer·협상·수신 버퍼 경계를 포함하고 원본 v1 호환 대조를 유지한다.

[1.1.32 증분](releases/1.1.32.md)에서 상태 section, FAT/NAND export, LocalMP/GDB 입력의 세 제한 블록을 통합했다. Windows 607개 회귀, 실제 Qt 복구, 수정하지 않은 원본 14.0 생성 상태 이관이 통과했다. 파일별 보존을 폴더 전체 복구나 모든 상태·게임 호환으로 확대하지 않는다.

후속 항목은 아래 순서와 첫 산출물로 넘긴다. 하나의 제한된 수정·필요한 검증이 끝나면 다음 독립 블록을 시작하고, 외부 수락을 기다리는 항목은 근거와 재개 조건을 남긴다.

| 순서·소유 블록 | 기존 ID | 다음에 만들 반례·대조 | 완료 또는 재개 조건 |
|---|---|---|---|
| 1. 그래픽 | GR-06/07/08 | Compute 실제 producer의 workload 초과·indirect 한도, GL alpha와 texture 끝의 capture 검색 | 작업을 버리지 않는 결과 보존과 실제 backend 픽셀 대조; 한도 미도달은 반증으로 기록 |
| 2. JIT/ISA | CJ-06/16/18 | 기존 guest fixture를 이용한 빈 PUSH/POP·조건부 사이클·비정렬 메모리 비교 | interpreter/생성 코드 대조와 문헌 의미를 구분; ARM64 native·실기 타이밍은 해당 실행 환경에서 재개 |
| 3. 배포 결속 | BV-01/05/18 | 다른 source revision의 EXE를 현재 소스와 묶으려는 입력 | 빌드 시 source 신원과 package 입력을 연결해 불일치 거부; 동일 EXE·문서만 변경한 경우의 정책도 명시 |
| 4. 통신 안정성 | NP-01/02/03, FS-17 | timestamp 0~31 reply, host 큐의 시각 wrap·가변 대기·늦은 peer; 먼저 자동조절 없이 계측 | host ms/guest μs 분리, 정상/구버전 대조와 취소 응답성; 자동조절은 아래 계약과 관측 후 판정 |
| 5. 성능 기준선·컴파일러 | BV-08/09/10/12, CJ-18 | 동일 입력의 cold/warm compile·CPU 실행·GPU 전송/대기 비용, 학습 밖 workload | JIT/PGO/SIMD 후보를 병목별로 분리; LTO guard를 근거 없이 해제하지 않고 동일 결과·median/tail을 확인 |
| 6. 저장 후속 | FS-02/06, CJ-10 | export 충돌의 사용자 복구 경로·폴더 삭제 충돌·ROM/GBA/firmware 부가 길이 | 1.1.32 파일별 보존 유지, 읽기/동기화 추가 I/O 비용 계측; whole-directory·동시 writer·다른 파일시스템은 별도 범위 |

[추가 참고 자료](Reference_Notes_2026-09-11.md)는 Qt/Dolphin의 저장 오류 처리, ENet의 소유권, QEMU/Dynarmic과 원 논문의 차등 검증, Khronos/parallel-rdp의 한도·가시성·작업 분할을 위 ID에 연결한다. 자료 수집 완료와 코드 적용·반례 해결을 구별한다.

[최적화 조사](Optimization_Research_2026-09-11.md)는 계측·컴파일러·JIT·GPU·멀티코어·네트워크의 후보를 중복 ID에 합쳐 정리한다. 먼저 BV-10/CJ-18의 비용 분리와 BV-09의 대표 workload/holdout을 준비한다. 직접 block chaining·x64 cycle register·GPU upload 재사용은 해당 병목이 확인될 때 독립 블록으로 넘기며, 공유 CMake와 release 문서는 한 작성자가 통합한다.

[1.1.33](releases/1.1.33.md)은 Compute 실제 workload/indirect 초과의 순서 보존 분할, 클래식 GL alpha와 텍스처 끝의 캡처·dirty word 경계를 구현했다. 그래픽 후속에는 blending을 끈 Compute 반투명 출력 차이의 정상/실패 대조, 큰 장면의 분할 비용, native texture fallback의 품질·성능을 남긴다. 다음 개발 묶음은 이 작은 반례와 JIT/ISA·배포 source/EXE 결속을 독립 소유로 진행한다. 게임·실기·다른 OS 수락은 위 로컬 수정과 구분한다.

2026-09-12의 [1.1.34 구현](releases/1.1.34.md): Compute는 DISP3DCNT bit3을 무시해 합성을 끈 반투명 폴리곤·A5I3 texel을 배경과 섞었다. 실제 source-A 캡처→guest LDRH의 기존 32조합 중 16개가 실패했고, bit3 검사 후 합성 켜짐·꺼짐·배경 alpha0을 포함한 44조합이 통과했다. 클래식 GL은 alpha16에서 정수 기준 `0x81D0` 대신 `0x81F0`을 내던 8조합을 별도 정수 합성으로 수정했다. 기존 batch의 영역을 GPU의 별도 텍스처에 복사하고 6비트 RGB와 5비트 alpha로 합성한다. 세 렌더러 각각 alpha44조합과 겹침·낮은 밝기·bitmap 배경24조합이 같은 기대값을 통과했다. 새 GL 저장 공간의 할당 실패 시 캡처 보존·software 복구·재시도도 통과했다. 픽셀당 4바이트 추가 저장과 GPU 영역 복사 비용이 있어 성능 향상을 주장하지 않는다. JIT는 A64 생성 코드28/28과 x64 실제 cached dispatch를 포함한40/40을 통과했다. 낮은 modulate alpha와 fog는6비트 출력22조합 및 최종 캡처 소비자로 추가 검증했다. 배포 source/EXE 결속(BV-01/05/18), Windows 전체631/631과 실제 Qt 복구는 통과했다. 실제 게임·다른 장치 수락은 후속이다.

[1.1.35 구현](releases/1.1.35.md)은 위 JIT/ISA와 통신 후속을 진행한다. 빈 Thumb PUSH/POP은 ARM 문서상 UNPREDICTABLE이며 빈 LDM/STM의 동작을 추정해 이식하지 않는다. 실제 interpreter·cached x64·fastmem의40관측은 정확성 통과가 아니며, 정상 LR-only/PC-only60대조는 통과했다. 인위적 C=4의 ARM7 LR-only에서 interpreter13/JIT10사이클 차이도 관측했으므로, 그 메모리/파이프라인 조건의 근거를 먼저 확인한다. 비정렬 메모리와 native A64·실기 의미도 후속이다.

LAN은64비트 host 경과 시간과 남은 대기를 사용하고, 새 참가자의 첫 유효 응답에는 기존 비활동 대기를 갱신한다. 중복·다른 MP 트래픽으로 계속 연장하지 않으며64MP 처리 뒤 부분 결과를 반환한다. 정상적으로20ms·40ms에 나눠 응답한 참가자를 전체25ms 한도로 자르지 않는다. 변경하지 않은1.1.34 소스39/44와 수정본44/44 대조에서 정상 느린 응답은 양쪽 모두 통과했다. 다음 통신 산출물은 대기 사용량·새 응답/중복·빈 반환·큐 나이·처리 상한의 관찰 전용 계측과 제한된 지터/손실/재정렬 입력이다. 자동조절은 그 증거와 복귀 정책을 정한 뒤 적용한다.

[1.1.36 구현](releases/1.1.36.md)은 ARM7 LR-only 차이를 전체 guest 주소의 `DataRegion >> 4` 판별 오류로 좁혔다.24비트 영역 판별로 고쳐 기존 timing 식을 올바른 메모리 영역에 적용했고, 다섯 profile·두 정렬의 interpreter/JIT/fastmem과 실제 cached 코드로 검증했다. 무조건3사이클을 더한 수정이 아니며 다른 영역 대조에서는10→9로 줄어든다. 실기 절대 timing과 비정렬 메모리·native A64는 후속이다.

LAN은 수신/큐/만료/거부·실제 host 대기·새 응답/중복/부분 반환/상한을 관찰하는128바이트 통계와 기존 로비 표시를 추가했다. 대기 정책과 guest timing을 유지한다. 같은 생성 경로의 양수 대기 계측 CPU 비용은 중앙값 약58ns 증가했으며 실제 소켓·게임 FPS·UI 비용의 증거는 아니다. 다음 독립 블록은 제한된 지터/손실/재정렬과 큐 나이·빈 반환·느린 참가자 진행 대조다. 이어 cold/warm JIT·CPU/GPU 비용 기준선과 holdout, export 복구·ROM 길이 경계를 진행한다. 자동조절은 여전히 미구현이며 먼저 유효 표본·악화/초기화·기본값 복귀 계약을 검증한다.

### 설정 없이 안정적으로 연결하기

NP-01/02/03과 FS-17의 후속 목표는 인터넷·PC·참여자 상태가 달라도 기본 설정으로 안정적으로 연결되는 것이다. 모든 환경에서 최저 지연을 보장한다는 뜻은 아니다. 먼저 host 수신 대기·큐 적체·지터·끊김을 관찰하고 게스트 Wi-Fi의 시간·IRQ 의미와 구분한다. 자동조절은 측정값과 검증된 제한 범위가 생긴 항목부터 적용한다.

- 기존 v1 형식과 구버전의 연결 절차를 유지한다. peer 협조가 필요한 확장은 지원 확인 뒤 사용하며 미지원·협상 실패 시 기존 경로로 돌아간다.
- 관찰만 하는 단계로 시작한다. 나쁜 한 번의 표본이나 절전·프레임 정지·peer 합류/이탈에 과민하게 반응하지 않도록 유효 표본·변경률·히스테리시스·복귀 조건을 명시한다.
- 대기 시간이나 큐만 늘려 끊김을 숨기지 않는다. 지연 상한·메모리 상한·취소/종료 응답성을 함께 검증하며, 상태가 나빠지거나 관측이 부족하면 검증된 기본값으로 복귀한다.
- 한 PC의 빠름을 기준으로 상대를 탈락시키지 않는다. 느린 peer, 가변 RTT·손실·재정렬, 대칭/비대칭 연결, 구버전 혼합을 포함한 같은 입력에서 기본값보다 악화되는 조건을 찾는다.
- host transport 개선을 DS의 가상 무선 timing 변경으로 대체하지 않는다. 프레임·패킷·연산 생략이나 emulated clock 변경이 필요하면 별도의 동작 변경으로 판정한다. 실제 두 PC·실제 게임은 생성 패킷 검증 뒤에도 남는 수락 조건이다.

먼저 넘길 산출물은 현재 계측·조절 지점 목록과 제한된 네트워크 상태 재현이다. 새 프로토콜, NAT 우회·중계 서비스, 게임 내 무선 메뉴 생략은 이 요구만으로 자동 도입하지 않는다.

1.1.32에서 발견한 `timestamp - 32` unsigned 경계는 [1.1.33](releases/1.1.33.md)에서 LocalMP·LAN의 안전한 차 비교로 수정했다. 실제 큐·loopback의 timestamp 0, age 32/33·미래·빈 AID0 응답을 대조했다. 다음은 host 큐 시각 wrap과 관찰 전용 계측·늦은 peer·취소 응답성이다. guest 32μs 창을 host 자동 대기 정책으로 바꾸지 않는다.

1.1.34에서는 host 하위 32비트 시각 0~15와 wrap 직전 수신한 정상 LAN 큐 패킷이 폐기되는 반례도 확인했다. 실제 수신 stamp와 public 수신 API를 통해 일반/command16입력을 대조하고 unsigned 경과 시간 비교로 기존 16ms 상한을 유지했다. 기존 packet37개와 새 통합 case, 실제 ENet 루프백이 통과했다. control event 처리 중 `time < time_last`의 조기 반환·남은 대기 budget은 이 큐 수정과 별도이며, 취소 응답성과 관찰 전용 계측의 다음 입력으로 남긴다. Windows 최종 통합은631/631을 통과했으며 이 결과가 실제 두 PC·혼합 구버전 통신 수락을 대체하지 않는다.

DSP 부족 출력·IRQ·채널 정렬, 실제 두 PC/구버전 혼합 LAN, 물리 오디오, Android·native A64·다른 OS·장기 게임 수락은 계속 열린 상태다. 실기 또는 해당 환경이 없다는 이유로 위 로컬 반례들을 함께 미루지 않는다.

core/DSP·GPU·연결/저장 파일을 독립 소유로 묶고 공용 CMake·프런트엔드 load·release 연결은 한 담당자가 순차 통합한다. 정해진 파일/명령의 수집과 검증·확정된 구현은 경량 작업자에게, 설계·타이밍·원인 판정은 총괄 또는 고성능 작업자에게 맡긴다. 배정 모델과 중단 뒤 실제 재개 모델을 구분하며 배정만으로 코드 검토 완료를 세지 않는다. 실기·native A64·플랫폼 수락을 기다리는 동안 독립 로컬 작업을 계속한다.

## 작업 소유권

EmuInstance.cpp를 건드리는 상태 로드·ROM 교체·Zstd 변경은 한 작성자가 순차 통합한다. 코어 NDS.cpp와 Config.cpp의 좁은 수정은 독립적으로 진행할 수 있다.
마이크와 입력 장치, LAN과 PCap/Slirp와 GDB는 독립 반례·소유 파일로 나눈다. 같은 파일의 동시 변경이나 같은 결과를 중복 구현하지 않는다.

반례 해결 뒤 기존 관련 테스트를 실행한다. 에뮬레이션 결과가 바뀌는 경우에만 interpreter·warmed JIT·GPU·게임 비교를 해당 경계로 넓힌다.
실기 명령 의미·DSP 타이밍·플랫폼 지원처럼 독립 oracle이 필요한 항목은 미검증 상태를 유지한다. 그 대기를 이유로 재현된 파일 보존 수정을 지연하지 않는다.

2026-09-12, 1.1.37: LAN 로비에 가장 오래 기다린 수신 패킷의 나이를 표시하며 기존 통신·만료·설정 정책을 유지한다. 생성 패킷의 재정렬·누락/늦은 응답·프레임 정지 뒤 큐를 확인했고, 비정렬 메모리의 정의된 ARM/Thumb 동작을 interpreter·cached x64 JIT·fastmem으로 검증했다. Windows 통합 640/640(포트 충돌을 피한 638+2 분할 실행, 실패·건너뜀 0)이 통과했다. 원본 클라이언트는 기존 5초 단일 대기에서 실패하고 같은 총 제한의 짧은 대기 반복 대조에서 재전송 후 연결됐다. 이는 수정하지 않은 원본의 호환 통과가 아니며 첫 UDP 미수신 원인·실제 두 PC 게임은 후속이다. 포켓몬 블랙의 반사광 픽셀 차이는 사용자 캡처로 확인했으나 동일 저장 상태/원본 대조 전이므로 그래픽 수정에 포함하지 않는다.

2026-09-12, 1.1.38: CJ-18/BV-10의 첫 제한 장면을 계측하고 JIT의 임시 배열 7개를 최대 32명령의 고정 용량으로 바꿨다. 설정·추적 길이·hash 입력은 유지한다. GCC x64에서 동적 스택 검사 호출 7→0, 고정 스택 예약 2096바이트이며 작은 block 설정에서는 예약량이 늘 수 있다. 동일 상태 120프레임·9쌍의 비계측 대조는 첫 frame 중앙값 9.4626→9.3478ms, warm 중앙값 4.9723→4.9259ms였지만 범위가 겹치므로 FPS 개선으로 판정하지 않는다. 별도 phase 계측에서는 cold 후보가 느리게도 측정됐다. 이 장면의 hash 약 0.046ms와 native 생성 약 3.36ms는 timer 영향을 포함한 120frame 합계이며 reset/reuse·holdout은 후속이다. 기존 ROMSmoke에 내장 DS BIOS·state 재생·선택적 frame CSV를 통합했다. CSV는 RunFrame 경과 시간으로 오디오 전달·최종 GL 대기와 구별한다. 세 렌더러의 동일 저장 장면 출력은 각자의 이전 결과와 바이트 단위로 같았으며 입력 파일 보존·120행 CSV·상태 오류·DSi 조합 거절·CSV 열기 실패를 검증했다. Windows 전체 640/640(실패·건너뜀 0)이 통과했다. 반사광 점은 흰 texel 처리 후 마지막 AA에서 coverage 6/32로 아래 적색과 섞이는 과정까지 재현했으나, 실기 및 실제 원본 프로그램 대조는 남아 있어 그래픽 계산은 수정하지 않았다. 다른 OS/native A64·장기 게임·실제 두 PC 수락은 별도다.

2026-09-12, 1.1.39: FS-02/19의 생성 입력으로 firmware의 할당 상한보다 큰 read 요청·Qt 오류값 수용, BIOS의 짧은 read/크기 불일치 성공·64비트 길이 축소, firmware 쓰기 거절 handle 누수, NDS의 padding이 숨기는 실행 데이터 누락을 재현했다. 펌웨어 파일은 512KiB 초과를 읽기 전에 거절하고 read 1개 완료를 요구한다. 기존 상한 이내 padding과 정상 128/256/512KiB는 유지한다. DS/DSi BIOS도 정확한 크기와 전체 읽기 후에만 적용한다. NDS는 원래 길이로 실행 영역을 검사한 뒤 padding하므로 미사용 끝부분만 잘라낸 정상 ROM은 허용한다. 기존 GBA 경계와 정상/실패 파일·내장 BIOS를 포함한 red→green 및 Windows 전체 643/643(실패·건너뜀 0)이 통과했다. 블랙 3모드와 솔라토로보 시작 장면 출력·사용자 입력 파일 보존도 확인했다. 과거 DSi 부팅 증상의 원인으로 단정하지 않는다.

BV-08은 GCC16.2/MSYS2에서 진단 전용 LTO core/ROMSmoke 및 실제 Qt 프로그램 빌드에 성공했고, 생산 guard는 유지했다. 같은 JIT/SIMD 설정의 Software 블랙 상태 120frame·솔라토로보 시작 1800frame을 각각 5쌍, 순서를 번갈아 CPU0에 고정해 대조했다. 모든 최종 출력은 같았다. 첫 frame/이후 frame 중앙값/p95/p99의 반복 중앙값(ms)은 블랙 9.5510/5.0182/5.7359/6.0552→9.2155/4.9360/5.4597/5.9833, 솔라토로보 11.1279/1.9468/3.4269/3.7497→12.4641/1.9364/3.3250/3.6663이었다. 솔라토로보 첫 frame 범위도 10.9505~11.3377→12.2109~12.6235로 늘어 기본 활성화 근거로 삼지 않는다. RunFrame만의 시간이며 파일 로드·오디오 전달·최종 GPU 대기·장기 플레이 성능을 포함하지 않는다. PGO 학습/검증을 수행한 결과도 아니다. 다음은 이 cold 비용의 원인 분리와 더 긴 workload, LTO Qt 실행 복구·native 플랫폼 및 compiler 조합별 판정이다.

2026-09-12, 1.1.40: FS-02의 FAT 폴더 동기화에서 guest 삭제를 호스트로 반영하기 전에 기존 내용 hash·종류·새 하위 파일을 검사한다. 충돌은 보존하고, 삭제 실패는 오류로 반환하며 같은 이미지의 다음 동기화에서 재시도한다. 재개 시 pending 삭제를 호스트 import보다 먼저 처리한다. 실제 FatFs·Windows 파일로 내용 수정/새 파일/종류 교체/삭제 잠금/인덱스 저장 실패와 정상 삭제를 검증했다. 읽기 전용 파일의 정상 삭제 및 실패 시 속성 복원을 유지한다. 사용자가 선택한 루트 별칭은 허용하며, MinGW가 하위 링크를 일반 폴더로 보는 반례에는 Windows reparse 속성 확인을 추가했다. 관련 43개 검사(실패·건너뜀 0)가 통과했다. 폴더 전체 원자성·검사 후 동시 writer 경쟁·복구 UI는 후속이다. hash 없는 구형 인덱스의 모호한 pending 삭제는 임의 승인하지 않는다.

BV-08 진단에서 LTO의 첫 frame 비용은 JIT 비활성 상태에서도 관측됐고, phase 계측은 ARM9 실행을 주요 구간으로 지목했다. ARM.cpp만 native object로 대체한 LTO 진단은 솔라토로보 초기 2frame·3쌍에서 첫 frame 중앙값 12.4169→11.4040ms(일반 11.2056ms)였다. 이후 블랙 120frame·솔라토로보 1800frame을 일반/후보 각각 5쌍 비교해 출력과 입력 보존을 확인했다. 첫 frame 중앙값은 블랙 9.9401→9.9318ms, 솔라토로보 11.1064→11.3431ms지만 반복 변동이 커 속도 개선으로 판정하지 않는다. 제품 LTO guard/CPU 코드는 유지한다. 다음은 조건을 통제한 장기 workload와 전체 Qt 실행·compiler별 검증이다.

1.1.40 최종 Windows 빌드·전체 657/657(79.63초, 실패·건너뜀 0)·배포 PE 129개 누락 import 0·clean PATH 시작이 통과했다. 별도 Qt 사용자 조작·실기·다른 OS 검증으로 확대하지 않는다.

2026-09-12, 1.1.41: LAN의 긴 수신 대기를 최대 25ms씩 나눠 종료 요청을 확인한다. 전체 정상 대기와 새 참가자 응답의 기존 갱신 정책, v1 패킷 형식은 유지한다. 실제 loopback의 1000ms 설정에서 종료 대기는 baseline 1000.284/999.979ms에서 0.139/24.954ms로 줄었고, 80ms 정상 timeout·60ms 지연 수신·재접속·4인 mesh가 통과했다. 생성 패킷 검사 51/51과 수정하지 않은 원본 8bdb579f LAN host/current client의 명령·응답·확인 및 timestamp 일치도 통과했다. 이는 자동조절 구현이나 두 PC의 실제 게임 수락이 아니며, 이전 원본 client 단일 대기의 첫 UDP 미수신 원인은 여전히 미확정이다.

FS-06은 종료 전 DSi/DLDI SD 폴더 동기화 실패를 Retry/Cancel/복구 이미지 선택에 연결한다. 같은 저장소로 재시도하며, 복구는 64KiB씩 읽어 별도 이미지로 원자 저장하고 원본/인덱스 별칭을 거절한다. 복사 성공은 폴더 동기화 성공으로 처리하지 않는다. 실제 FatFs 이미지와 Qt 위젯에서 내용 보존·취소·자식 인스턴스·재시도·복구 실패/재로드를 검증했다. 위젯 fixture의 생산자/EmuInstance는 대역이므로 실제 앱 종료·물리 장치 수락과 폴더 전체 원자성·동시 writer는 후속이다.

1.1.41 최종 Windows 빌드·전체 CTest670/670(92.96초, 실패·건너뜀0)·배포 PE129개 누락 import0·clean PATH 시작 및 소스 신원 대조가 통과했다. 최초 통합 링크의 테스트용 경로 함수 누락은 PlatformHeadless의 기존 파일 조회 대역에 보완했다. 앞선 SD short-read seed 생성 실패는 무수정 단독 및 최종 전체에서 재현되지 않았으나 원인은 미확정으로 남긴다.

FS-06 후속: 1.1.41의 실제 Qt main/EmuInstance/EmuThread/MainWindow와 생성 ARM 반복 코드·FAT 입력으로 8개 앱 실행이 통과했다. DSi/DLDI getter, 모달120ms 동안 CPU/프레임 정지, Cancel 후 실행 재개, 재시도 종료·8MiB 복구 전체 일치/재로드, 첫 카드 복구 뒤 둘째 카드에서 취소해도 원래 충돌 유지까지 확인했다. 생산 수정은 없었다. Software 표시·단일 실제 인스턴스의 검증이며 게임 부팅/guest SD 드라이버 부하·다중 인스턴스와 폴더 전체 원자성은 남는다.

2026-09-12, 1.1.42: LAN 자동 대기는 모든 준비된 상대의 서로 다른 최신 ENet ACK 표본과 RTT/변동을 사용한다. 설정값을 하한·복귀값으로 유지하고 100ms 상한, 초당 최대10ms 변경과 5ms 완충 구간을 적용한다. 신규/기존 기본25ms는 자동, 기존 사용자 지정값은 고정으로 이관하며 설정 창에서 끄거나 변경할 수 있다. 0ms 즉시 조회와 100ms 이상 고정값은 유지한다. 관측 부족·상대 변경·오래 멈춘 guest 수신·신뢰성 있는 명령의 높은 손실·연속3회 실제 수신 만료에서는 복귀한다. ENet 손실 값은 MP 데이터그램 손실률이 아니며, 처리량 상한에 따른 부분 반환을 수신 만료로 세지 않는다. v1 패킷·guest timestamp·큐의16ms 만료·새 상대 응답의 기존 대기 갱신은 바꾸지 않는다. 실제 설정 창의 Cancel/Apply/재시작·고정 모드 복원4회와 생성 패킷 검사가 통과했다. 실제 두 PC 게임·다른 OS 수락은 후속이다.

실제 ENet/loopback relay의 고정0/20ms·비대칭5/35ms·지터·4회 응답 누락·재정렬/복제 6조건을 각각20회 대조했다. 첫 수신 성공은 1.1.41에서20/0/0/5/0/0, 현재에서20/20/20/20/16/20이었다. 모든 명령·수신 payload/timestamp와 소유 프로세스 종료를 확인했고 의도적4손실을 성공으로 세지 않았다. 처음 검사 도구의 응답 후 반복 blocking drain이 가짜 만료를 만들었으므로 raw nonblocking drain으로 고쳐 양쪽을 다시 대조했다. baseline의4조건에서 각각1개 늦은 응답을 최종 관측하지 못한 결과도 보존하며 원인을 확정하지 않는다. 빠른 연결에서도 추정 대기 상한은25ms보다 커질 수 있고, 누락 시 기다리는 비용이 늘 수 있다. 실제 수신은 도착 즉시 반환하며 이 실험을 게임 FPS나 인터넷 전체 성능으로 해석하지 않는다.

1.1.42 Windows 전체 빌드와 CTest670/670(포트 분리668+2, 72.14+10.63초, 실패·건너뜀0), 원본8bdb579f host/current client의 생성 교환, 배포PE129개 누락0·clean PATH 시작·소스 식별 대조가 통과했다. 다음은 BV-05의 실제 재현된 임의 사용자파일 ZIP 포함 문제다. 정확한 배포 파일 목록을 입력으로 사용하고 나머지를 제외·알리며, 기존 소스 식별 검사를 유지한다. 이번 새 배포 폴더는 이전 검증 ZIP의365개 불변 파일과 새 EXE/README/launcher만 있음을 별도 대조했다. 장기 workload·실제 두 PC 게임·실기 AA·native A64/다른 OS와 기존 미완료 항목은 유지한다.

2026-09-12, 1.1.43: BV-05의 기존 packager는 알려진 확장자 외 임의 txt/json/jpg 등 생성5파일을 그대로 포함하면서 private_files_in_zip=0으로 보고했다. 이제 신뢰한 배포 입력에서 만든 필수 --runtime-manifest의 정확한 상대 경로/SHA-256만 압축한다. 추가 파일은 내용 해싱 없이 제외·개수 경고하고, 지정 파일의 누락/변조·경로 alias/외부 해석·잘못된 목록·runtime/입력 목록과 출력 충돌을 거절한다. 최종ZIP의 정확한 목록·내용 hash를 다시 검사하며 알려진 개인 파일 규칙의 개수와 전체 제외 개수를 구분한다. 기존 source identity 거절과 생성 runtime 전체 보존을 검증했다. Windows 대소문자 표기만 다른 유효 파일을 제외 개수에 중복 계산하던 후보 반례도11→10으로 수정했다. 이는 목록 생성자의 입력 신뢰를 자동 판정하거나 모든 동시 filesystem 변경을 원자화하는 기능은 아니다.

BV-02는 버전 정책을 한 CMake 파일에 모아 표시의 명시적 선행0을 유지하되 Windows RC 숫자에서는 제거한다. 기존1.1.0191은 windres가 경고와 함께 numeric1.1.137.0으로 만들었고 수정 후1.1.191.0이다. 0/9/09/10/99/100/0191/65535의 실제PE8조건과65536의 WORD 범위 초과 거절을 확인했다. 처음 overflow 검사 실패는 CMake 오류 문구의 줄바꿈 비교 문제였으며 해당 조건만 바로잡아 검증했다. 다른 compiler·OS 및 전체 UI/파일 정렬 수락으로 확대하지 않는다.

1.1.43 통합: Windows 빌드와 버전 경계를 포함한 전체671개 중670개가 첫 실행에 통과했다. PackageIdentity의 생성 저장소에 Python3.14가 만든 __pycache__가 들어가 정상 입력을 dirty로 거절한 실패를 재현했고, 실제 저장소와 같은 캐시 제외 규칙을 fixture에 적용해 해당1개를 재검증했다(79.00초 전체+18.76초 해당검사, 남은 실패/skip0). 제품의 dirty/source identity 거절은 유지한다. 테스트 소스 변경에 맞춰 실행 파일 식별값을 갱신했으며 배포PE129개 누락0·clean PATH·실제 버전1.1.43/숫자1.1.43.0·명시적368파일 목록과 소스 식별을 확인했다. 다음은 BV-03/04의 기존 배포 도구와 실제 의존성 복사 입력을 연결해 이 목록을 안전하게 생성하는 로컬 profile이다. 실제 두 PC/장기 게임·native A64/다른 OS·실기 AA 및 앞선 미완료 항목은 남는다.

2026-09-12, 1.1.44: BV-03/04의 기존 msys-dist.sh는 UCRT64 의존 경로를 선택하지 못해 DLL64종이 빠졌고 exit0 뒤 실행은0xC0000135였다. 새 로컬 배포 도구는 Qt의 명시적 파일 목록과 CMake의 전이 import를 사용하고, 복사 입력의 hash로 packager용 목록을 만든다. 기존 출력·누락/충돌·변조는 거절하며 버전이 붙은 EXE와 launcher 이름을 맞췄다. 설치 라이선스21개 누락 판정 중 ICU/Qt2개는 실제 설치 위치·동일 source/version으로 해소했고, 나머지19패키지의 공식 원문40개는 명시적 버전/hash 추가 입력으로 보존한다. 고지의 법적 완전성이나 전체 대응 소스 제공을 자동 인증하지 않는다.

Windows 전체672/672(52.89초, 실패/skip0), 실제 배포322파일의 입력/hash 대조·PE129개 누락0·clean PATH의 EXE/launcher 실행·배포 폴더 qwindows 로드·1.1.44 버전/소스 식별 일치를 확인했다. 검증 도구의 환경 변수 키와 Windows Qt 로그 출력 위치 문제는 도구에서 수정했으며 제품 실패로 집계하지 않는다. 다음은 BV-07의 toolchain/의존성 및 local patch 출처 대조다. 실제 두 PC/장기 게임·다른 OS/native A64·실기 AA와 앞선 미완료 수락은 유지한다.

2026-09-12, 1.1.45: BV-07의 libarchive 준비 도구는 두 번째 다운로드/해시 실패 전에 첫 기존 사용자 patch를 덮어썼다. 기본 실행을 검증·차이 보고로 바꾸고, 모든 pinned 파일·VCPKG license·release hash와 순차 patch 적용을 확인한 뒤 명시한 새 출력에만 후보를 만든다. 기존 출력은 거절하고 현재 overlay는 유지한다. 생성4그룹과 실제 공식8입력·3.8.9 소스의 patch 적용을 검증했으며 현재7파일과 생성 결과는 개행 차이 외 동일하다.

설치 도구5개·패키지90개·배포DLL128개의 hash/소유자·실제 링크 명령을 연결했다. 별도 probe가 실제 배포 DLL을 로드해 SDL2 2.32.10·libarchive3.8.9·zstd1.5.7·ENet1.3.18·Qt6.11.2·FAAD2 2.11.3을 설치 정보와 대조했다. FAAD 헤더의 unknown은 그대로 기록했고 첫 SDL_main 링크 실패는 probe의 진입점 선언을 수정했다. vendored7개는 포크 기준 tree/patch를 보존했으며 Teakra·libslirp2개에 변경이 있다. 이는 각 라이브러리의 원 upstream revision 인증이나 재현 빌드가 아니다. Windows 빌드·673/673(57.79초, 실패/skip0), 배포322파일·기존과 같은DLL128개·EXE/launcher/배포qwindows·버전/소스 식별이 통과했다. 다음은 BV-06의 실제 사용 중인 표준 기능과 지원 compiler/STL의 불일치를 확인한다. BV-07의 원 upstream revision/대응 소스와 장치·다른 OS·실기 수락은 후속으로 유지한다.

2026-09-12, 1.1.46: BV-06은 현재 GCC16.2/libstdc++에서 실제 사용처 compile과 bit/byteswap/span/stop_token/to_underlying/checked arithmetic 관련8개 실행이 통과해 제품 변경을 하지 않았다. GCC14·Clang/libc++·MSVC·다른 OS는 미검증이다. BV-13은 기존 RelWithDebInfo EXE를 보존하고 새 폴더에 GNU debug 정보와 실행 파일을 분리한다. debuglink CRC·양쪽 hash를 확인하며 기존 출력/정보 없는 입력/도구 실패는 성공 manifest 없이 거절한다. 기본적으로 입력 EXE를 실행하지 않고 명시 --build-info에서만 내장 메타데이터를 대조한다. 5개 행동 그룹과 실제 Qt 진단 빌드에서 실행 코드/시작 동등·NDS::RunFrame 주소의 NDS.cpp 줄 복원이 통과했다. 일반 Release 설정은 유지하며 실제 crash/ASLR/JIT unwind·다른 OS 수락으로 확대하지 않는다. Windows 전체674/674(58.08초, 실패/skip0).

GR-08의 새 편지 상태는 DSi·DSP HLE였다. 같은 입력의 3모드600프레임과 선택 커서가 생기는 추가 입력을 대조해 확대 마지막에 Software/Compute의 반사광 점이 고정되고 Classic에는 없는 것을 확인했다. 원인은 해당 위치에서 첫 흰 조각이 더 가까운 붉은 표면 아래에 기록된 뒤, 다음 흰 조각의 삽입으로 아래층 색이 바뀌어 마지막 AA에 빨강이 섞이는 경로까지 좁혔다. 추적270프레임은 기존 출력과 일치했다. 실기에서는 확대 중 점이 보이고 선택 화면에서 사라진다는 사용자 관찰/사진을 확보했으나 동기화된 디지털 픽셀 oracle은 없으며, 그래픽 제품 코드는 유지한다. 다음은 해당 깊이 보간/겹친 경계의 최소 실기 재현과 BV-07 원 upstream revision·대응 소스 연결이다.

1.1.46 배포 검증: 명시된322파일·PE129개 누락0, 이전 배포와 같은DLL128개 import 근거 재사용, clean PATH의 EXE/launcher 및 배포 qwindows 로드·버전/소스 식별 일치. 원본 사용자 자료와 기존 배포는 유지했다.

2026-09-12, 1.1.47: GR-10의 16비트 bitmap 변경 감시 범위가 실제 바이트 수의 절반이어서 뒤쪽을 바꿔도 GL 화면이 이전 내용을 유지했다. guest VRAMC와 flat cache는 정상인 반례를 확보하고 2바이트 단위를 반영했다. 128/256 크기·1x/2x·8비트 대조 및 네 면 홈브루의 첫 실행/재실행 출력이 일치한다. GR-08 홈브루는 게임과 같은 깊이 차이112/AA6/32 경로를 재현하며 Software/Compute의 AA OFF FFFF·ON94FD와 Classic FFFF·FFFF를 표시한다. 실기 값과 캐시 비용/상주량은 별도 미완료다. BV-07은 BUILD에 Teakra/libslirp 원본 revision을 연결했고 상속/fork patch6단계가 현재 tree와 일치한다. 이후 libslirp fuzz 입력31개의 잃어버린 symlink 의미 복원을 검토하며, 이는 runtime C 버전 갱신 근거가 아니다.

1.1.47 검증: Windows 빌드·전체677/677(57.67초, 실패/skip0), 실제 블랙 편지→선택600프레임의 세 모드별 이전 출력 일치와 개인 입력9개 hash 보존을 확인했다. 최종 core의 홈브루 첫 출력도 정상 재실행 기준과 일치한다. 배포322파일·PE129개 누락0, 같은DLL128개 import 근거 재사용, clean PATH의 EXE/launcher·배포 qwindows 및 버전/소스 식별이 통과했다. 실기 결과·다른 GPU/OS·장기 게임은 미완료다.

2026-09-12, 1.1.48: CJ-11 활성 예외 모드의 debugger SPSR 접근이 CPSR를 읽고 쓰던 결함을 수정했다. ARM9/7·5 bank·활성/비활성20조건은 interpreter/JIT에서 각각10/20→20/20이며 MRS와 MOVS 예외 복귀로 저장 상태의 실제 사용을 확인했다. p/P의 XML wire 번호와 내부 enum 불일치도 수정하고 완전한 길이·hex 검증 뒤에만 접근한다. 기존 XML/g/G 순서는 유지한다. 실제 TCP RSP의 XML 조회·p/P·g 교차 검증은 양 CPU에서 실패→통과했고 GNU ARM GDB UI 수락은 남는다.

BV-07은 원본 libslirp v4.8.0 alias31개를 실제 packet 사본/디렉터리로 저장해 소스 ZIP 소비 대상8/16→16/16, 전체19 corpus·61 packet 바이트 일치를 확인했다. source identity의 symlink 거절은 유지하며 향후 원본/사본을 함께 갱신해야 한다. POSIX 실행·fuzz campaign·스크립트 실행 bit 복원은 수행하지 않았다. Windows 전체 빌드와 CTest682/682(59.78초, 실패/skip0)가 통과했다. 다음은 실기 AA 값 판정과 CJ-12 DMA 타이밍의 실제 입력/기준 대조이며 native A64·다른 OS·장기 게임과 앞선 미완료 과제는 계속 남는다.

2026-09-12, 1.1.49: AD-03 생성자의 bitdepth 인자 덮어쓰기와 Auto 기종 조건 불일치를 기존 SetDegrade10Bit 경로 재사용으로 수정했다. 비공개 입력 없이 실제 DS/DSi 코어에 반복 PCM16을 공급하여 Auto/10/16비트의 최초 생성·재설정 출력을 setter 경로와 대조했고 3/6→6/6을 확인했다. Windows 전체 빌드·CTest682/682(79.95초, 실패/skip0)가 통과했다. 실제 오디오 장치와 청취 수락은 별도다. 구형 13.0 상태 가져오기는 GPU·scheduler·cart 구조와 과거 DSi 전원 의미의 변환 실험까지 진행했으며 제품에는 아직 넣지 않았다. 일반 저장 시점, DMA/SPI 진행 중 상태, 1.1로의 역방향 내보내기는 후속으로 남긴다.

2026-09-12, 1.1.50: CJ-01의 A64 곱셈 가변 I 누락·중복 fetch, signed early termination·long accumulate·Thumb의 원래 Rd 기준을 수정했다. [ARM7TDMI timing 표](https://documentation-service.arm.com/static/5e8e1323fd977155116a3129)와 실제 emitter 실행을 대조하면서 interpreter/x64의 추가 오류도 재현했다. Interpreter716조건은125실패→0, 캐시된 native x64는348실패→0, production A64를 Unicorn으로 실행한404조건도 최종0실패다. ARM7 multiply의 정의되지 않은 C는 oracle에서 제외하며, native A64 ABI/메모리 보호·실기 timing을 통과한 것으로 세지 않는다.

CJ-12는 기존 [DS 버스 실측](https://melonds.kuribo64.net/board/thread.php?pid=3805#3805)에 따라 GBA slot128KiB 마지막 halfword의 N 접근 비용을 반영한다. 실제 DMA 데이터·timestamp·IRQ136조건은80실패→0이며 mainRAM overlap·선점·ITCM 동시 실행은 남는다. CJ-16은 [VirtualFree 계약](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree)에 맞춰 동적32MiB JIT slice의 해제 인자를 수정했다. Windows x64의3회 누수가 모두 해제되고 native 코드18회 실행과 기본 슬롯 재사용이 유지됐다.

GR-09는 software2D의 효과별 scanline 분기·정수 RGB6 합성과 불필요한 두 번째 층 처리를 줄인다. 독립 scalar/baseline과18,550,784픽셀이 일치했고, 같은 최종 코어에 렌더러만 교체한 실제 블랙 편지→선택600프레임도 모두 일치했다. 생성5장면의 paired scanline wall 중앙값은3.02~6.82% 감소했다. 실제 게임600프레임의8쌍은 총 RunFrame 비용 변화가−17.70~+17.35%로 넓게 흔들려 전체 게임의 속도 향상은 확정하지 않았다. CJ-10은 카트 checksum/type/삽입 유무 불일치를 실제 로드 오류로 전파해 기존 rollback을 실행하며, 정상 로드·다음 frame 보존5조건을 검증했다. 상태 형식은14.2를 유지한다.

Windows 통합 빌드·전체 CTest693/693(93.02초, 실패/skip0)이 통과했다. 구형13.0 일반 가져오기·1.1 역방향 저장, 실기 AA·native A64·다른 OS·장기 게임 수락은 미완료다. 다음 독립 작업은 ARM9 cache/ITCM, x64 BMI, software3D·SPU 비용, libslirp 갱신이다. JIT 자기수정 코드의 추가 검증은 미완료다.

1.1.50 배포는322파일의 원본/hash·PE129개 누락 import0·기존과 같은DLL128개와 clean PATH의 EXE/launcher·배포 qwindows 로드·버전/소스 식별 일치를 확인했다. 개인 입력은 보존하며 패키지에 포함하지 않는다.

2026-09-12, 1.1.51: CJ-13은 ARM946E-S TRM 4.1.1에 따라 MPU를 끈 상태의 접근을 I/C enable bit와 무관하게 noncacheable로 처리한다. 실제 guest MCR·외부 fetch/load/store·MPU 전환·독립 TCM 대조60조건에서 interpreter/native x64 JIT/fastmem 각각26실패→0이다. 캐시 정책 변경을 가로지르는 기존 block·native A64·실기 절대 timing과 전체 cache 모델은 미완료다. GR-09는 software3D의 R/B 정수 합성을 묶어 곱셈6→4로 줄였다. 생성112프레임의 색/깊이/속성 및 같은 최종 core의 블랙600프레임이 기존 계산과 일치한다. 생성 AA 장면12쌍의 CPU cycle 중앙값8.13%, wall6.73% 감소는 해당 커널 조건의 결과이며 전체 게임 FPS 개선으로 확대하지 않는다.

BV-07은 출처를 고정한 libslirp4.9.4를 통합했다. 기존23개 공개 symbol·이전 header consumer·61개 corpus packet을 보존하고, 마지막 IPv4 fragment의 header 길이가 다른 실제 echo 누락을 수정한다. 최종 생산 Net_Slirp object와 vendored archive의 두 fragment 입력·runtime4.9.4를 확인했다. 호스트 UDP loopback의 첫 datagram 미수신은 이전/현재 및 독립 송수신에서도 남아 있어 해결로 세지 않는다. 실제 게임 Internet·다른 OS 수락은 후속이다. SPU pan과 BMI1 BIC 후보는 출력 검증과 별개로 성능 근거가 불충분하여 제품에 넣지 않았다.

Windows 통합 빌드·전체 CTest696/696(84.75초, 실패/skip0), 배포322파일·PE129개 누락 import0·기존과 같은DLL128개·clean PATH EXE/launcher/배포 qwindows·버전/소스 식별이 통과했다. 다음은 최대20개 독립 작업으로 오디오 전달/커널·입력/표시 지연·CPU/OS ISA dispatch·DSP·저장/플랫폼 호환을 분담하며, 원본1.1/현재 upstream과의 개선 및125개 계획의 구현·수락 상태를 별도로 대조한다. 개별 실행 검사 통과를 물리 지연·실기 일치·구형 상태 일반 호환이나 전체 계획 완료로 세지 않는다.


2026-09-12, 1.1.52: 20개 병렬 작업의 구현 후보15묶음을 통합했다. CJ-02 x64 조건부 PC 쓰기 cycle, AD-06 SPU channel capture, AD-20 DSP PDATA IRQ mask, 거절된 savestate header 재읽기, 압축 texture palette cache의 끝4바이트 누락을 수정했다. Software/Legacy/Compute 실제 guest capture에서 palette 교체가 반영된다. 구형13상태 일반 이관·실기 capture/IRQ timing·블랙 AA 문제 전체 해결을 의미하지 않는다.

오디오 mute 해제는 기존1ms recovery ramp를 사용하고 SSE2 PCM의0.5 경계 반올림을 맞췄다. 일시정지 중 제어 메시지 대기는 queue wake로 끊고, GL swap interval은 각 창을 current로 만든 뒤 적용한다. Qt의 오른쪽 modifier E0 scan code·GL header 기본 include·FMA4 OS 상태 판별도 바로잡았다. NEON RGB6 변환은 little-endian compiler baseline일 때만 자동 선택하며 기존 enum/낮은 ISA fallback을 보존한다. Compute clear 통합은 batch당 dispatch1회와 shader1개를 줄인다. payload 없는 archive의 전체 크기 선할당도 피한다. 실제 전체 FPS·입력/오디오/화면 물리 지연 향상은 아직 확정하지 않는다.

Windows 전체 빌드가 통과했다. 검사 프로세스 중단 전에 완료된635개 기록과 미완료64개의 후속 통과를 합쳤고, 기존 microbenchmark 옵션을 켜 반올림 verify1개만 추가했다. 현재 등록700개 모두 성공, 실패/skip0이며 성공한 full suite를 반복하지 않았다. 최종 core로 블랙 편지→선택600프레임을 세 renderer에서 실행했고 개인 입력/NAND/state hash를 보존했다. native ARM/macOS/Linux/Android, 물리 장치/장기 게임, 실제 두 PC 통신 및 기존 미완료 과제는 남는다.

125개 ID의1.1.51 고정 감사는 최소 구현·로컬 검증68, 좁은 계약 수락2, 부분28, 미착수22, 후보 보류/조건부/제외5다. 1.1.52의 AD-06/20 최소 증분2개와 FS-20 부분 착수를 반영하면 각각70/2/29/19/5다. 과제 크기가 다르고 실기/플랫폼 수락이 남아 있으므로 제품 완성도 백분율로 쓰지 않는다. 원본1.1과 현재 master의 상류 기능을 자체 개선으로 세지 않으며, 별도 릴리즈 문서가 없는37이후 기록도 이 실행 기록에서 이어진다.

다음은 보통2~4개의 필요한 독립 작업만 배정한다. Apple native SIMD/SME 호출 비용과 실제 처리 병목은 공식 자료·참고 프로젝트에서 조사하고, 출력이 NUL로 확정된 Windows GUI 로그의 불필요한 formatting 생략부터 작은 후보를 검증한다. 치트 구조 검증 재사용은 불변 실행본 소유권 확인 뒤 판단한다. C/C++ 표준 변경이나 assembly 사용 자체를 성능 개선으로 세지 않는다.

2026-09-12, 1.1.53: Windows GUI 시작에서 stdout의 NUL 전환이 성공한 경우에만 Platform::Log의 formatting을 생략한다. 콘솔/file/pipe·전환 실패·직접 help/build-info 출력은 유지한다. DeepSeek의 작은 후보를 실제 제품에 통합했고 현재 UCRT64의 생성29조건 및 기존 실제 console5조건을 확인했다. C23/C++26은 이미 사용 중이며 이 변경을 언어 표준 갱신 성과로 세지 않는다.

AD-04/05의 SPU 출력 소비는 기존 QMutex를 유지한 채 stereo sample별 루프를 링 양쪽의 최대2회 memcpy로 묶는다. 읽기 위치를 lock 안에서 한 번 반영하며 producer의 overflow/drop·queue 크기·resampler/filter·guest schedule을 보존한다. 추출한 실제 함수의324조건에서 반환/PCM/위치/원본 링이 일치했고, 기존 실제 DS/DSi 생성 PCM의 잘게 나눈 read·wrap·출력 guard·clock/skew/drop/drain 회귀가 통과했다. Windows 빌드와 전체700/700(73.13초, 실패/skip0)이 통과했다. 기존1.1.52 게임/영상 증거는 변경되지 않은 guest/render 범위에만 재사용하며 새 물리 오디오 지연 검증으로 세지 않는다.

후속은 AArch64 FP64 L/R NEON 오디오의 명시적 후보와 native 검증이다. 자동 선택은 실측 전 유지한다. 픽셀 SIMD는 실제256픽셀 호출과 기존 전체화면 벤치의 차이를 반영해 측정해야 한다. libyuv의 locally-streaming·ZA 미사용 픽셀 커널, Opus의 채널 SIMD 순환 필터, mGBA의 분할 링 복사·audio wait predicate를 참고했다. native QWidget repaint→update 병합은 정상 표시 지연 악화 가능성이 있어 실측 전 보류하고, audioSync의 제어 요청 wake는 잠금/취소 수명을 검증할 별도 후보로 남긴다. 원본 상태/실기/다른 OS/실제 통신 및 기존 미완료 계약을 완료로 올리지 않는다.

SPU 복사 비용은 모든 작업자·빌드/검사를 멈춘 구간에서 같은 QMutex와 실제 추출 함수, 고정 CPU의12조건·9쌍으로 비교했다. 128~1024프레임 조건은 모두9/9 개선 방향이었다. 512프레임의 호출 중앙값은 연속 구간0.345→0.036µs, wrap0.341→0.046µs였다. 링 위치 준비/검사 비용도 포함한 단일 스레드 결과이고 동일 함수 대조군도 약±11% 흔들렸다. 작은16/64조건의 정밀 향상률·producer 경합·전체 callback·물리 지연으로 확대하지 않는다. 장치 queue 크기는 바꾸지 않았다.

2026-09-12, 1.1.54: AD-04/05의 audioSync가 기존 제어 요청 stop token을 받아 큐 대기를 끝내도록 연결했다. callback은 SDL mutex를 잡기 전에 등록하고 잠금을 푼 뒤 해제하여 이미 취소된 token과 대기 진입 사이의 신호를 놓치지 않는다. 기존 큐 threshold·500ms 장치 starvation fallback·PCM은 유지한다. 기존 FrontendAudio 검사에 실제 SDL wait의 사전 취소·반복 취소·spurious wake·정상 소비를 추가했다. 같은 검사에서 이전 함수는 사전 취소 및 두 제어 요청에 실패했고 수정본은 통과했다. 이것은 큐 대기 중 제어 응답 개선이며 물리 오디오 지연 측정이 아니다.

ARM 오디오에는 AArch64 FP64 두 lane으로 좌우 채널을 계산하는 명시적 NEON backend를 추가했다. 기존 enum 값·Auto 선택·scalar fallback·필터 단계/시간 순서·mute 및 coefficient ramp를 유지하며 streaming/SME 명령은 사용하지 않는다. 실제 Android Clang A64 ELF를 Unicorn에서 실행한 1,800개 전환 블록·952개 경계 사례에서 PCM 차이0, strict profile의 모든 FP64 state가 일치했다. 기본 compiler profile의 state는 미세한 차이가 있어 bit exact로 세지 않는다. ARMv7/비지원/ENABLE_SIMD=OFF에서는 제외되며 명시적 NEON 검사는 비지원 시77을 반환한다. native Apple ABI·기기 성능·물리 출력 검증은 남아 있고 자동 선택을 승격하지 않았다.

Windows 전체 빌드·CTest700/700(73초, 실패/skip0)이 통과했다. 변경된 frontend 대기·x64 회귀를 확인했고 ARM runtime probe와 별도로 기록했다. 다음 우선순위는 실제256픽셀 호출 크기의 SIMD 측정, native ARM 오디오 및 입력/출력 장치 수락, 기존 ROM 취소·설정 저장 실패·13형식 상태 호환 과제다. 계획 ID 완료 수를 이번 후보 추가만으로 올리지 않으며 GitHub Actions·macOS 빌드는 사용하지 않는다.

2026-09-12, 1.1.55: 사용자 승인 후 CJ-15의 기존 작업을 같은 에이전트에서 재개했다. CompileBlock이 해석 실행 중 자기 명령을 바꿔도 해당 블록은 아직 코드 감시 범위에 등록되지 않아 다음 실행에 이전 명령을 사용했다. 실제 물리 쓰기 주소를 tracing 중 기록하고 블록 게시 직후 기존16바이트 무효화 경로로 재확인한다. store 부작용·CPU/Thumb tag·hash guard를 유지하며 전체 cache flush는 추가하지 않았다. 현재1.1.54 기준의 interpreter/JIT/fastmem24조건에서 JIT와fastmem각8실패→0을 확인했다. 확장80조건씩에서 ARM9/7·ARM/Thumb·미러 쓰기/실행·STR/STRH/STM·일반 데이터 쓰기·이웃 블록 보존과 최적화OFF의 값/PC/CPSR/guest cycle이 일치한다. 두 최적화ON의100기능 조건도 통과했다. native A64·모든 remap/코드 writer·강제 코드 버퍼 소진과 전체 guest timing 인증은 남는다.

FS-04는 Config::Save의 성공/오류 반환과 GUI 재시도/저장 없이 계속하기를 연결했다. 설정 창·LAN 설정·최근 목록·창/앱 종료는 같은 실패 안내를 사용하고, 배경 RTC 저장 함수에는 GUI를 넣지 않는다. 같은 실제 입력 설정 창에서 생성한 QSaveFile 실패가 기존2조건에서 조용히 닫히던 것을 재현하고, 원본 보존·재시도·현재 세션 유지·나중 저장·새 프로세스 재로드를 확인했다. 손상 TOML 저장 차단과 성공 시 오류 초기화도 유지한다. 전원 손실·디스크 소진·전체 설정 동시성 재설계를 완료로 세지 않는다.

기존 PixelConvertBenchmark는 전체 프레임1회 호출과 실제 렌더러 크기인256픽셀×192회 호출을 분리한다. 동일한 생성 입력·프레임 memcpy를 포함하고, 모든 지원 backend의 두 출력 전체를 Scalar와 비교한다. timing 밖의 검사·9회 workload/backend 쌍 shuffle·검사 전용 실행을 사용한다. 이 변경 자체는 커널 최적화나 전체 게임 속도 개선이 아니며 Auto 선택을 바꾸지 않는다.

Windows 전체 빌드 후 CTest는704/705가 통과했고, 변경하지 않은 SaveManagerIO의 recovery-copy1개는 Qt 파일 교체에서 액세스 거부로 실패했다. 그1개만 분리 실행해 통과했으며 원인 미확정인 최초 실패 로그와 전체 결과를 보존했다. 현재705개 모두 통과 증거가 있으나 최초 전체 실행을 무실패라고 쓰지 않는다. 최종 core로 블랙 편지→선택600프레임을 세 renderer에서 실행했고 마지막 화면은1.1.52의 대응 결과와 동일하다. 개인 파일 hash를 보존했다. 다음은 구형13상태 일반 이관·ROM 준비 취소·SMC의 나머지 remap 및 실제 장치/플랫폼 수락을 이어간다. GitHub Actions·macOS 빌드는 사용하지 않는다.

최적화ON에서 interpreter와 다른35/100 cycle 관측은 같은 fixture·128cycle 예산의1.1.54 baseline에도 동일했다. 기준/후보의 cycle은 JIT·fastmem각100/100 일치하고 새 cycle 변화는0이다. 기대한 두 번째 SMC 실행의 r0 수정16조건 외 PC/CPSR/메모리 변화는 없다. 이것으로 기존 cycle 차이의 정확성까지 해결한 것은 아니다.

측정은 작업자·컴파일·검사 종료 후 현재 Windows 빌드로 수행했다. 256픽셀×192회 호출과 memcpy를 합한 프레임 중앙값은 이 Ryzen7 9800X3D에서 Scalar10.90µs, AVX2 4.43µs, AVX512BW3.08µs, AVX512F4.02µs였다. 9회 섞은 순서의 생성 입력 측정이며 같은 코드 대조군·다른 CPU·native ARM·전체 게임/물리 지연의 성능 인증은 아니다.

2026-09-12, 1.1.56: CJ-15의 DMA·SWRAM/NWRAM 재매핑 재진입을 보강한다. 이전 물리 메모리를 가리키는 실행 lookup과 ARM9 직접 명령 읽기 view를 비우고, 같은 가상 주소의 블록을 교체할 때 이전 물리 인덱스에서 그 블록만 제거한다. 이전 backing에 DMA로 쓰면 교체 블록이 지워지거나 해제된 블록을 다시 참조하던 경로를 수정하며 이웃·다른 CPU/ISA·alias의 코드와 페이지 보호는 유지한다. ARM9 view 정리는 JIT가 없는 빌드에도 적용한다. 기준1.1.55의 생성 재현에서 interpreter12개 실패와 JIT/fastmem 각각16개 remap timeout을 확인했고, lookup만 고친 중간 후보에서는 힙 손상을 재현했다. 최종 후보의 완료된 기준/수정 RUN1728쌍은 guest cycle이 같았으며 기준이 멈춘432행과 실기 절대 timing에는 이 결론을 적용하지 않는다. 남은 검증은 native A64·실행/trace 중 remap·NWRAM 겹친 페이지 broadcast·교차 영역 literal·강제 코드 버퍼 소진이다.

최종 Windows 빌드와 CTest756/756은134.77초에 통과했고 실패·skip은0이다. 새 DMA·재매핑51개 시나리오와 기존 SMC·DTCM·MPU 회귀가 포함된다. JIT OFF 별도 빌드도17개 시나리오848검사를 통과했고 기존 ARM9 ARM/Thumb의 각6실패가 사라졌다. 같은720개 guest 실행 행의 전후 cycle이 일치한다. 블랙 편지→선택600프레임을 세 renderer에서 실행한 최종 화면은1.1.55와 같고 개인 입력 파일을 보존했다. 배포322파일·PE129개의 import와 clean PATH 실행·source/runtime ZIP을 확인했고 fork master deb33eb2의 반영을 재조회했다.

FS-04는 시작 시 설정 로드 오류에 파일 경로·구문 위치 또는 읽기 실패 이유를 전달하고 기존 오류 창의 상세 정보로 연결했다. 알려진 TOML·파일 스트림 예외를 처리하고 실패 시 이전 설정과 원본을 보존하며 성공적인 재로드 뒤 오류·저장 차단을 해제한다. 기존 FrontendInput에서 손상 TOML·경로가 디렉터리인 실제 parser I/O 실패·쓰기 거부·복구를 확인했으며 최종 전체 검사에 포함됐다. 이 증분은 전원 손실이나 전체 동시성 수락을 뜻하지 않는다.


2026-09-12, 1.1.57: CJ-10은 KEY1 명령 상태를 새 console/cart에 복원할 때 누락되던 파생 키를 BIOS·게임 코드·DS/DSi 모드로 재구성한다. 생성 입력의 실제 MMIO·scheduler·전체 상태에서 양 CPU의 암호화 chip ID가0으로 나오던 실패를 재현했고 DS/DSi 각각2실패→0을 확인했다. 정의가 없던 DSi::DoSavestate 선언도 제거하여 공통 상태 API와 DSi 추가 section을 상속하며 실제 NWRAM 복원을 확인했다. 상태 버전/레이아웃14.2는 유지한다. 실물 BIOS/펌웨어 부팅 수락 및 원본13형식 일반 가져오기·역방향 저장은 별도다.

FS-20은 일반 ROM 읽기·archive 목록/추출·Zstd 준비를 별도 스레드로 옮긴다. 기존 parser/카트 교체·저장 소유권 선택을 유지하고 준비 완료의 세대/취소 token을 확인한 뒤 적용한다. 취소·재선택·종료와 저장 파일 충돌 선택 중의 늦은 완료를 거절하며 기존 카트/대기 카트·세이브·최근 목록을 보존한다. 생성 지연 IO의 기존 UI 정지1.35~1.39초를 재현했고 수정본의 Qt 이벤트 처리와 취소·정상 적용을 확인했다. 이 수치는 생성 입력의 결과이며 실제 매체 성능이나 전체 GUI 지연 수락은 아니다. OS 자체가 멈춘 read는 반환 후 합류하고, ParseROM·세이브/FAT 준비는 여전히 기존 적용 경로에 있다. 카탈로그와 모든 준비 단계의 비동기화를 완료로 세지 않는다.

Windows 최종 통합 빌드와 CTest786/786이168.93초에 통과했고 실패·skip은0이다. 새 준비/취소29조건·KEY1 복원과 기존 SMC·DSi·저장 회귀를 포함한다. 통합 중 DSi 부팅 및 메시지 테스트의 옛 함수 인자를 갱신했으며 최초 빌드 실패 로그를 보존했다. 최종 core의 블랙600프레임을 세 renderer로 실행해1.1.56의 대응 마지막 화면과 일치하고 개인 파일을 보존했다. native ARM·실제 오디오/표시/저장 장치·구형 상태 일반 호환·실제 두 PC 통신 수락 및 나머지 공개 계획은 남아 있다. GitHub Actions·macOS 빌드는 사용하지 않는다.


2026-09-12, 1.1.58: CJ-15 실행/trace 중 SWRAM·NWRAM 재매핑에서 낡은 번역 블록을 계속 실행하던 결함을 수정했다. 기존 guest prefetch를 보존하고 CPU 레지스터·플래그·cycles를 반영한 뒤 native 블록에서 빠져나온다. ARM9/7·ARM/Thumb·분기/연속·cold/warm·분기 최적화 ON/OFF의112조건씩에서 JIT/fastmem은 각28→112통과, interpreter112통과다. WRAM 초기값·저장 상태 매핑 복원 및 일반 JIT load의11사이클 회귀도 수정했다. native A64, 겹친 NWRAM broadcast, 교차 영역 literal, 강제 cache 소진과 실기 timing은 후속이다.

CJ-10은 원본1.1의 형식13 상태를 직접 가져오도록 GPU·scheduler·DSi·카트리지 레이아웃을 이관했다. 이미 버퍼에 들어온 ROM 읽기와 진행 중 쓰기를 완료까지 보존하며, 해당 구형 전송이 남은 재저장만14.3을 사용한다. 일반 저장은14.2이고 역방향1.1 저장은 지원하지 않는다. DSi 클록·시간값·실효 RAM 크기·슬롯 연결을 복원하고, 잘린 SRAM payload가 live 저장 메모리를 교체하지 않도록 했다. 원본에 없는 중간 그래픽 이력은 복구할 수 없어 가져오기 성공 시 일부 화면 차이를 안내한다. 최종 코어의 원본 생성 상태256조건은 통과했다. 별도 홈브루 SD 쓰기22조건도 원본의 전체 데이터·상태·IRQ와 같았으나 C0 읽기2조건의 선행 이벤트 예약은 달라 엄격 비교22/24이며 사이클 동등성을 주장하지 않는다.

Windows 최종 빌드·전체 CTest793/793(80.75초, 실패/skip0)을 통과했다. 블랙의 현행14.2 및 사용자 원본1.1 slot2 상태를 각각 세 renderer에서600프레임 실행했고, 같은 renderer의 최종 화면은1.1.57과 일치했다. 원본1.1 코어와 직접 대조한 것은 software 최종 화면이며 그 결과도 같다. ROM·BIOS·NAND·state 원본 hash를 보존했다. 이전 JIT 단독 후보의 한 장면 측정8.741→8.782초(+0.47%)는 전체 성능 향상을 뜻하지 않으며 이번 전체 배치의 새 벤치마크도 아니다. 모든 구형 장치/통신 timing, 실제 게임 장기 실행·물리 지연·native 플랫폼 및 나머지 계획은 미완료다. GitHub Actions와 macOS 빌드는 실행하지 않는다.

2026-09-12, 1.1.59: CJ-15에서 DTCM·I/O 등 추적 불가 literal을 상수화하다 충돌하던 결함, WRAM 재매핑 후 다른 영역의 번역 코드에 남던 상수값, ARM9 경계 prefetch의 잘못된 메모리 view, ARM7 경계 native2사이클 차이를 수정했다. 재매핑 가능한 RAM을 상수로 참조하는 블록을 무효화하며 재컴파일 뒤 상수 최적화를 유지한다. DSi NWRAM 겹침 쓰기는 각 실제 물리 페이지를 무효화하여 읽기 view 뒤에 가려진 페이지의 낡은 코드까지 제거한다. 새로운 제품 소스 파일 없이 기존 실행 경로를 수정했다.

상수 읽기72조건/실행모드, 겹침204사례/실행모드의27,720검사와17,424개 전체 레지스터·PC·플래그·사이클 상태가 통과했다. 겹침 기존 JIT/fastmem의 각2,448실패가0이 됐다. 실제 생성 코드를 실행하여 일반 JIT61,752·fastmem26,910블록 뒤 자동 캐시 리셋을 관찰했고, 리셋 뒤 양 CPU·ARM/Thumb의 재컴파일·SMC·이웃 블록 보존89검사씩 및24상태씩이 통과했다. 캐시 위치를 조작하거나 ResetBlockCache를 직접 호출하지 않았다. 이 검사는 far 버퍼와 native A64 소진을 인증하지 않는다.

Windows 최종 빌드와 전체 CTest799/799(89.87초, 실패/skip0), JIT OFF 영향 범위14/14가 통과했다. 블랙의 현행14.2와 원본1.1 slot2를 세 renderer에서 각각600프레임 실행하여 같은 renderer의 마지막 화면이1.1.58과 같음을 확인했고 개인 입력 파일 hash를 보존했다. 새 성능 향상·모든 프레임/장치 timing 동등성을 주장하지 않는다. native A64·교차 영역 인라인 명령의 재매핑 의존성·실기 절대 timing 및 나머지 계획은 후속이며 GitHub Actions·macOS 빌드는 사용하지 않는다.

2026-09-12, 1.1.60: CJ-15의 교차 영역 인라인 명령 재매핑 의존성을 추적한다. 진입점의 backing이 유지돼도 포함된 WRAM 명령의 backing이 바뀌면 해당 블록을 분리하며 이웃 블록과 상수/분기 최적화를 유지한다. DSi 확장 RAM enable 변경을 코드/host 매핑에 반영하고 ARM7 상위 private RAM에 겹치는 NWRAM을 분류한다. 64KiB private RAM host view가 다른32KiB backing을 덮지 않도록 DSi에서는 반쪽별로 매핑한다. 매핑 변경 때 WRAM host view를 다시 조회하며 전체 코드 캐시 flush는 하지 않는다.

고정1.1.59 대조의 교차 코드64조건에서 JIT/fastmem 각16개의 잘못된 레지스터/PC/플래그 상태가0이 됐고, 기존256개 guest timestamp/cycles는 유지됐다. interpreter와는 각64개의 기존 branch timing 차이가 남아 cycle 동등성을 주장하지 않는다. 추가 SCFG enable 재현에서 JIT50실패·fastmem51실패, private RAM 인접 읽기 재현에서 fastmem2실패를 확인했다. 최종 기존 실행 fixture의73사례/실행모드가 통과했고, Windows 전체802/802(73.98초, 실패/skip0), JIT OFF 영향 범위25/25도 통과했다. 현행14.2 및 원본1.1 slot2 블랙 상태를 세 renderer에서 각600프레임 재생한 마지막 화면은 같은 renderer의1.1.59와 같고 개인 파일 hash도 유지됐다.

far 코드 버퍼의 실제 용량 소진/자동 리셋 후 SMC는1.1.59 고정 코어에서 받은 근거를 보존했다. 1.1.60 연결 재실행은 서브에이전트 자동 안전검사가 차단하여 미실행이며 현재 버전 통과에 포함하지 않는다. native A64, 실기 절대 timing 및 나머지 공개 계획은 남는다. GitHub Actions와 macOS 빌드는 사용하지 않는다.

2026-09-12, 1.1.61: CJ-14의 ARM9/7 Timer0~3 기반 NDMA 시작을 구현했다. 기본·연쇄 타이머 overflow에서 같은 CPU의 선택된 NDMA 채널을 시작하며 타이머 IRQ enable과 분리한다. NDMA 채널을 켜기 전에 경과한 타이머 이벤트를 기존 비활성 상태로 먼저 처리해 과거 이벤트로 소급 시작하지 않게 한다. 기존 DMA 전송·완료 IRQ·우선순위 소비자를 재사용하고 새 저장 필드나 이벤트 슬롯은 추가하지 않았다. 계약은 [GBATEK DSi NDMA](https://problemkaputt.de/gbatek.htm#dsinewdmandma)의 timer 시작 모드와 총/논리 블록 정의를 따른다.

기준1.1.60의 생성56조건에서448검사 실패를 확인했고, 시작만 연결한 중간 후보의 과거 overflow 재현에서는112검사가 실패했다. 최종60사례/실행모드에서 양 CPU·타이머4개·연쇄·IRQ on/off·ARM9 클록 전환·짧은 최종 블록·완료/비활성·pending 저장/복원을 확인했다. 실제 guest STR로 채널과 타이머를 켜서 RunFrame으로 전송했으며 JIT/fastmem의 warm 재실행은 기존 블록 보존·코드 추가 생성 없음과 데이터/PC/IRQ를 확인했다. 전체 Windows805/805(80.58초, 실패/skip0), JIT OFF 영향 범위3/3이 통과했다.

현행14.2 및 원본1.1 slot2 블랙 상태를 세 renderer에서 각각600프레임 재생한 마지막 화면은 같은 renderer의1.1.60과 같고 개인 입력 hash를 보존했다. 타이머 요청은 기존 scheduler 단위로 처리하며 물리 DSi 시작 지연·subblock interval·round-robin·버스 중재·native ARM/실기 timing은 완료하지 않았다. CJ-04 겹친 MPU 갱신 후보는 분석 근거만 확보했고 제품 통합은 남는다. GitHub Actions·macOS 빌드는 사용하지 않는다.


2026-09-13, 1.1.62: AD-07의 PCM8/16·ADPCM one-shot HOLD를 구현한다. 마지막 sample 시작에서 Busy를 내리고 마지막 주기 끝까지 출력을 유지하며, HOLD이면 유지 출력을 계속한다. 재시작의 첫 주기 유지와 뒤따르는 무음 주기는 [GBATEK Sound Notes](https://fabiensanglard.net/another_world_polygons_GBA/gbatech.html#dssoundnotes)에 따라 보존한다. 수동 stop·HOLD 해제·reset은 잔여값을 정리하고 새 상태 필드나 포맷을 추가하지 않는다. 실제 mixer/resampler·capture 경로의10개 명시적 PCM 연속 파형 대조와6개96-sample 종료 timeline이 통과했다. 기존 버전의 HOLD on은 캡처가12288/-8192/4097 대신0이었고 Busy가두 half-period 늦게 내려갔다.

첫 HOLD 후보는 구형14.2의 수동중지 상태에서 남은 내부 sample을 재생했다. 이전 코어가 실제 MMIO·전체 savestate로 만든8개 상태를 대조하고, 자연 one-shot 마지막 구간을 제외한 비활성 채널의 잔여값을 불러오기 때 정리했다. 수정 후8개 캡처와1604-frame PCM이 이전 코어와 같고 새 held-state의 저장/복원도 통과했다. 구형 수동중지가 정확히 마지막 sample과 겹친 상태와 HOLD 중 format/repeat/length를 바꾼 상태는 저장 정보만으로 구분하기 어려워 AD-07/CJ-10 후속으로 남긴다. 실기·물리 오디오·sub-mixer timing 수락은 별도다.

CJ-19/GR-05/06은 RAM에서 준비한 같은 native load 블록으로 source A를 CPU-first/DMA-first 순서로 읽는다. 일반 JIT/fastmem·16/32비트·128/256폭·bank wrap·GL1x/2x의120조건에서 캡처960값과 준비 단계960값이 통과했다. 실제 VCOUNT polling·guest load/즉시 DMA를 사용하며 미완료 줄은 poison을 유지한다. 이 범위에서는 새 렌더러 결함이 재현되지 않아 제품 그래픽 코드는 유지했다. fastmem은 기존 VRAM 동기화 fallback을 사용하며 직접 host 매핑이나 성능 개선을 주장하지 않는다. 모든2D source A·subscanline latch·VCOUNT 변경·다른 driver/native A64·실기/게임은 후속이다.

최종 Windows 빌드와 전체806/806(87.13초, 실패·skip0), JIT OFF 오디오 영향 범위2/2가 통과했다. 배포322파일·PE129개의 import와 독립 PATH 실행도 확인했다. 이 증분은 오디오 의미 수정과 캡처 검증 공백 해소이며 전체 FPS·물리 지연 개선을 주장하지 않는다. GitHub Actions·macOS 빌드는 사용하지 않는다.

현행14.2와 원본1.1 slot2(형식13)의 블랙 상태를 세 renderer에서 각각600프레임 재생했고 같은 renderer의 마지막 화면은1.1.61과 같다. 개인 입력 파일 hash를 보존했다. 전체 게임 오디오·모든 상태의 일반 호환이나 모든 프레임의 timing 동등성으로 확대하지 않는다.

2026-09-13, 1.1.63: AD-07/CJ-10의 HOLD 중 설정 변경 후 저장/복원 손실을 수정했다. 기존 reader는 format/repeat/length/position으로 비활성 출력을 추정하므로 설정이 바뀌면 남은 소리를 버렸다. 이런 상태만14.4를 요구하고 출력·보간 이력을 그대로 복원한다. 일반 저장은14.2, 구형 카트리지 전송만 남은 저장은14.3이며 새 channel 필드는 없다. 14.4는 항상 NC13을 기록해 오디오 때문에 버전이 올라간 경우에도 카트리지 상태를 명시한다. 1.1.62 이하 reader는14.4를 거부하며 일반/구형 저장의 가져오기는 유지한다.

DS/DSi의24조건에서 기존14개 손실을 재현한 뒤 모두 통과했다. 실제 capture/Busy와 full-state를 사용하며14.4의 누락·모호한 NC13,14.3의 빈 NC13도 거부한다. 원본13 카트리지 상태를 가져와14.3→14.4로 재저장한 별도10조건에서 잔여 읽기/쓰기 데이터·완료 IRQ·다음 명령·PCM이 대조군과 같았다. 구형 수동중지8상태의 캡처와 PCM도 보존했다. 구형 stop이 정확히 마지막 sample과 겹친 상태는 여전히 구분할 수 없고 실기/native ARM·전체 게임 호환의 수락은 별도다.

최종 Windows 전체 검사는806/807(79.24초)이 먼저 통과하고, 기존 `save-manager-flush-latest`에서 파일 교체의 액세스 거부가 발생했다. 변경하지 않은 해당1건만 분리 재검증해 통과했으며 최초 OS 오류 원인은 미확정으로 보존했다. FS-06 후속으로 간헐적 파일 교체 거부 원인·실제 앱 복구 경로를 확인한다. assertion·제품 재시도 정책은 바꾸지 않았고 전체 검사는 반복하지 않았다. JIT OFF 오디오3/3은 통과했다. GitHub Actions·macOS 빌드는 사용하지 않는다.

현행14.2·원본1.1 slot2(형식13)의 블랙 상태를 세 renderer에서 각각600프레임 재생했으며 마지막 화면은 같은 renderer의1.1.62와 같다. 개인 입력 파일을 보존했다. 전체 FPS나 물리 출력 지연의 개선량은 측정하지 않았다.

2026-09-13, 1.1.64: CJ-09는 실제 G711 상태 factory로 callback을 재생성한 뒤 MMIO 명령·전체 상태 복원·RunFrame 결과를 대조한다. 중지한 core 복원, 진행 중 core 교체, 범위 밖/미등록 FuncID 거부 후 정상 재로드에서 PCM·응답·IRQ와 중복 완료 방지를 확인했다. callback 검증을 재생성 이전으로 옮긴 비공개 대조 변형은 정상 복원에서 실패했다. 프로그램 CRC 인식/부팅은 검증하지 않았으며 생성 DSP 상태 seed와 G711 v0x10 범위만 인정한다. 생산 core 수정은 없다.

FS-08은 생산 decoder의 실제1GiB 한계에서10조건을 실행했다. 크기 명시/미명시·취소 가능한 stream 경로·빈/skip frame 후속은 전체 출력 바이트가 일치하고,1바이트 초과·잘린/손상 checksum·쓰레기 후속은 원본 입력 owner와 바이트를 보존하며 거부했다. 상한 축소나 decoder 교체는 없고 실제 UI 취소 시각·ROM 파싱·다른 OS는 별도다.

FS-06은 실패 로그에 open/write/commit·경로·Qt 오류를 추가했다. 기존 저장13조건 중 buffer-resize의 commit 액세스 거부가 분리 실행에서도 재현됐다. 별도 관측에서 파일은 읽기 전용이 아니고 실패 직후 삭제 접근으로 열렸으며 Restart Manager 점유 조회는0건이었다. 실패 시점의 일시 점유/파일시스템 filter를 배제하거나 원인을 확정할 수 없으므로 해결로 표시하지 않는다. 자동 재시도·직접 덮어쓰기 fallback·assertion·저장 완료 판정은 바꾸지 않았다. 다음은 교체 거부의 정확한 실패 원인과 실제 앱 복구 수락이다.

최종 Windows 전체808/808(80.53초, 실패·skip0), 기존 SMC69개와 JIT OFF의 새 HLE 검증이 통과했다. 이번 전체 검사의 저장13개 성공은 앞서 두 차례 실패한 원인의 해결을 뜻하지 않는다. 전체 검사는 한 번만 실행했다. 현행/원본1.1 저장의 블랙600프레임×3 renderer에서 최종 화면과 개인 입력 보존이1.1.63과 같았다. 전체 animation 픽셀·실기 timing의 동등성을 뜻하지 않는다. GitHub Actions·macOS 빌드는 사용하지 않는다.

2026-09-13, 1.1.65: NP-18의 GBA Flash 섹터 지우기가 전달 주소부터4KiB를 변경하던 문제를 섹터 경계 정렬로 수정했다. 은행 선택을 실제64/128KiB에 맞게 제한하고, A0 뒤 바이트는 unlock/reset처럼 보이는 주소·값도 데이터로 처리한다. 읽기·쓰기·지우기는 물리 Flash 범위와 실제 버퍼 범위를 넘지 않으며128KiB 뒤의16바이트 RTC 부가 정보를 유지한다. 명령 우선순위·은행 값 검사·섹터 정렬은 [mGBA 생산 구현](https://github.com/mgba-emu/mgba/blob/master/src/gba/savedata.c)과 대조했고, 미정의 은행 비트나 물리 chip timing의 실측으로 주장하지 않는다.

기존1.1.64의64KiB·128KiB·RTC 부가 정보3크기×섹터/은행/바이트3그룹에서9조합 실패를 확인했다. 같은 경로의 수정 후보는 모두 통과했고, 별도로 정확한 크기의 저장 버퍼와 ARM9/7 실제 메모리 핸들러·슬롯 소유권6조합 및 JIT OFF도 통과했다. 기존 범위 초과는 여분의 소유 backing과 canary로 관찰했고, 프로그램 payload는 매번 지워진 backing에서 검사한다. Flash 모든 명령이나 CPU 명령 실행·EEPROM ROM bus/DMA·실기 latency 수락은 아니다.

FS-06의 구형 디버거 시작 실패는 보존하고 설치된 SDK GDB17.2로 통제한 파일 점유를 추적했다. NtSetInformationFile의 이름 교체가 C0000022를 반환하고 점유 해제 후 성공했다. 자연 발생 검증8회는 디버거 아래에서 재현되지 않았으며 breakpoint가 시각을 바꾸므로 기존 간헐적 거부의 원인은 계속 미확정이다. 시스템 권한·filter·다른 프로세스나 제품 재시도 정책은 바꾸지 않았다.

다음 NP-18 작업은 아직 없는 Flash chip-erase 명령, GBA SetSaveMemory의 길이 변경과 실제 할당 용량 일치, 잘린 GBA 상태의 메모리/저장 통지 보존이다. 이 가운데 가져오기·상태 경계는 현재 정적 후보이며 재현 후 수정한다. FS-05 RequestFlush의 할당 실패 시 길이 게시 순서도 정적 후보로 유지한다. EEPROM의 실제 ROM bus/DMA와 실물 chip 수락을 Flash 수정으로 완료 처리하지 않는다.

FS-03은 실제 Qt/EmuInstance/EmuThread 메시지와 생산 Platform 카트 저장 callback으로 DS3·DSi3·GBA SRAM1조건을 확인했다. B 파싱/자원 준비가 실패해도 기존 카트·세션·저장 소유자가 보존되고, 기존 pending 및 후속 카트 쓰기가 A 파일에만 반영된다. 고정1.1.64 검증 뒤 최종1.1.65에 재연결하여7/7과 입력1,089개 hash 보존을 확인했다. 생성 NTR 카트·FreeBIOS·NAND 없는 DSi이며 실제 TWL/게임·일반 UI 선택의 수락은 별도다. 생산 프런트엔드 수정은 없었다.

최종 Windows 전체812/812(90.76초, 실패·skip0), 기존 SMC69개와 JIT OFF의 GBA 버스 검증을 통과했다. 전체 검사는 한 번만 실행했고 GitHub Actions·macOS 빌드는 사용하지 않는다. 간헐적 Windows 파일 교체 거부는 이번 성공으로 해결 처리하지 않는다.

현행/원본1.1 상태로 블랙600프레임을 각각 세 renderer에서 실행했고, 마지막 화면은1.1.64의 대응 결과와 일치했다. 최종 core와 개인 입력 파일의 hash를 확인했다. 전체 animation 픽셀·실기 timing 및 다른 게임의 상태 호환으로 확대하지 않는다.

2026-09-13, 1.1.66: NP-18의 Flash chip erase 명령 누락을 재현하고 정상 unlock/erase 순서에서 물리64/128KiB 전체를 지우도록 구현했다. 선택한 은행과 RTC 부가 정보는 보존한다. 미완성 순서·잘못된 최종 주소는 지우지 않는다. [mGBA 구현](https://github.com/mgba-emu/mgba/blob/master/src/gba/savedata.c)의 명령과 범위를 대조했으며 실물 지우기 지연이나 WIP를 새로 구현한 것은 아니다.

GBA 상태의 데이터/명령 metadata가 잘리면 기존 메모리와 GPIO를 바꾼 뒤 저장 통지까지 보내던 문제를 재현했다. 임시 메모리와 장치 필드를 먼저 읽고, 기본/태양광 카트의 전체 GBCS를 검증한 뒤 반영한다. 태양광 tail이 잘리는 경우와 저장 callback이 복원 전 센서 상태를 관찰하던 중간 후보도 별도로 실패를 확인했다. undefined save type·잘린 slot header를 거부하고 할당 실패는 noexcept 경계 안에서 처리한다. 생성13/current 정상 상태의 바이트 순서·pending A0·은행·RTC와 빈 상태 복원을 유지했다. [원본1.1 GBCS](https://github.com/melonDS-emu/melonDS/blob/1.1/src/GBACart.cpp)와 필드 순서를 대조했으며 전체 콘솔의 다른 section이 뒤늦게 실패하는 경우까지 원자적으로 복원한다고 주장하지 않는다.

SetSaveMemory의 빈 버퍼 쓰기·기존 할당보다 큰 복사·겹친 자기 참조를 실제 NDS::SetGBASave→slot→cart 경로에서 재현했다. 새 버퍼를 먼저 할당/복사하고 성공 시에만 소유권과 길이를 바꾸며, 저장 callback에는 카트가 소유한 전체 데이터를 전달한다. bad_alloc은 기존 데이터와 통지를 보존한다. 기존 입력 길이별 SetupSave 판별과 미등록 길이의 경고/이전 type 유지 정책은 바꾸지 않았다. 종류 자동 추정·버퍼 크기에 맞춘 자동 절단으로 성공 처리하지 않는다. 비공개 관측은 실제 new[] 요청과 CRT 복사를 추적하며, 위험한 null/겹침은 기록 후 중단하고 범위 초과는 소유한 여분의 backing에서 관찰했다. 자연 발생 프로세스 crash나 실제 메모리 고갈로 확대하지 않는다.

다음 작업은 FS-05의 save worker 할당 실패 전 길이 게시, FS-06의 간헐적 Windows 파일 교체 거부 원인, NP-18의 실제 EEPROM ROM bus/DMA 소비자와 판별 정책이다. native ARM/실물 장치·실제 두 PC 통신 및 나머지 공개 계획 수락은 계속 남는다. GitHub Actions·macOS 빌드는 사용하지 않는다.

최종 Windows 전체 검사는814/816(88.75초)이 통과했고, 변경하지 않은 save-manager-replace/flush-latest가 commit 액세스 거부(Qt10)로 실패했다. 해당2개만 한 번 분리 실행하여 통과했으며 최초 실패와 원인 미확정 상태를 보존한다. 전체 무실패로 기록하지 않는다. GBA 관련8개·기존 SMC69개·JIT OFF의 GBA8개, 최종 실제 core에 연결한 import11조건과 상태 할당 실패1조건이 통과했다. 현행/원본1.1 블랙600프레임×3 renderer의 마지막 화면은1.1.65와 같고 개인 입력 파일 hash를 보존했다. 전체 animation·실기 latency나 기존 Windows 교체 거부의 해결을 뜻하지 않는다.


2026-09-13, 1.1.67: FS-05 저장 버퍼의 최초 확보·크기 변경 실패가 producer로 전파되고, 기존 버퍼보다 큰 길이를 먼저 게시하던 문제를 재현했다. 새 버퍼를 먼저 확보/복사한 뒤 소유권과 길이를 함께 반영한다. 실패하면 최신 데이터 미확보 상태를 유지하고 원본 저장·복구 사본의 성공 처리를 막는다. 다음 부분 쓰기도 놓친 전체 데이터를 다시 복사한다. 보조 버퍼 할당 실패는 프레임 경계에서 처리하고 기존 최신 버퍼와 저장 요청을 보존한다.

재확보는 실행 스레드의 프레임 경계 또는 producer가 정지한 종료·교체·리셋 경계에서 현재 코어의 소유 데이터를 읽는다. worker가 빌린 코어 포인터를 보관하거나 다른 스레드에서 읽지 않는다. 생성 firmware의 확장 AP+AP 저장 범위와 외부 firmware 전체 범위를 유지한다. 새 회귀9개와 전체 Qt의 DS/DSi/GBA 다음 프레임 복구3조건이 통과했고, 사용한1,090개 소스·라이브러리가 최종 빌드와 일치했다. 자연 발생 메모리 고갈·느린 저장장치·장기 경쟁 수락과 FS-06의 Windows 간헐적 파일 교체 거부 원인은 별도다.

NP-18 후속 조사에서 [GodMode9i의 ARM7 EEPROM 소비자](https://github.com/DS-Homebrew/GodMode9i/blob/8a8f806054133978e186d3a55d4058c551dde497/arm7/source/gba.c#L8)가 DS 모드 ROM 버스의 16비트 DMA3를 사용함을 확인했다. 고정1.1.66 코어의6실패 중 시제품은3개를 해결했지만 CPU 쓰기 오허용2개와512바이트 용량 판별1개는 실패한다. 생산 통합 전 실제 DMA/칩 선택 경계·긴 주소 판별·초기 용량·busy 및 진행 중 상태 복원 계약을 먼저 해결한다. 지나간 GBA CPU 모드의 규칙이나 DMA 길이만으로 칩 종류를 정해 통과시키지 않는다.


최종 Windows 전체 검사는824/825(92.25초)이 통과했다. save-manager-flush-latest의 commit 액세스 거부(Qt10)1개는 한 번 분리 실행해 통과했으나, 최초 실패·종료 코드8을 보존한다. 해당 파일 commit 함수와 기존 테스트 본문은1.1.66과 동일하며 원인은 미확정이다. 전체 무실패로 기록하지 않는다. 기존 SMC69개와 새 저장 복구9개는 전체 실행에서 통과했다. 현행/원본1.1 블랙 상태600프레임×3 renderer의 마지막 화면은1.1.66과 같고 개인 입력을 보존했다. 저장 형식은 추가하지 않았으며 이 결과를 전체 animation·실기 지연이나 모든 구형 상태 호환의 수락으로 확대하지 않는다. GitHub Actions·macOS 빌드는 사용하지 않았다.


1.1.67 이후 FS-06 진단: melonDS/core/SaveManager를 연결하지 않은 단일 프로세스에서 Qt 저장과 Win32 직접 교체를 대조했다. debugger 없이 제한한201회 관측(자체 점유 대조2회·비통제199회) 중 Win32 FileRenameInfo가 오류5를 반환했고 원본 전체 바이트가 보존됐다. Qt 분기의128회 성공은 원인 해결이나 영구 안정성의 증거가 아니다. 이 오류는 melonDS 저장 스레드나 JIT가 없어도 발생하지만, 과거 모든 실패의 동일 원인이나 특정 프로세스/필터의 책임을 입증하지는 않는다. 정확한 실패 시점의 거부 주체 추적은 남는다. 제품의 저장 방식·재시도·테스트 기대값·시스템 설정은 바꾸지 않았다.


NP-18의 기존512바이트 긴 주소 판별 실패는 고정6비트 칩에서 read dummy 값을 검사하지 않고 이후 쓰기 클록을 무시하는 동작으로 해결했다. 기존 기대 데이터·용량은 유지했다. [NanoBoyAdvance 고정 칩 구현](https://github.com/nba-emu/NanoBoyAdvance/blob/55b5cf0ae3d929582ac5bfd486558173502b8354/src/nba/src/hw/rom/backup/eeprom.cc#L76) 등 생산 비교와 원래 실물 관측을 대조했으며 다른 chip 변종의 동일 동작까지 주장하지 않는다. 실제 DMA의 CPU/채널·전송 폭·주소·시작/종료 정보를 사용한 비공개 시제품에서 기존3실패와 추가 경계10조건이 통과했고, CPU 간 독립 DMA를 잘못 중단하지 않도록 수정한 영향2조건도 통과했다(14고유조건,15실행). scheduler yield와 실제 전송 종료를 구분하며 DMA3/특정 주소/패킷 길이의 용량 추론은 넣지 않았다.

다음 NP-18 구현은 시제품의 관측용 RTTI 연결을 그대로 반영하지 않고 실제 slot/cart의 전송·장치 상태로 옮기는 것이다. main RAM 중재·32비트/고정 주소·ARM9의 칩 선택, 초기 chip 용량·busy 시간, reset/import/교체/저장상태 소유권을 해결한 뒤 생산 통합한다. 현재16비트 WRAM·18/6 timing 밖의 시제품 거부는 실물 동작으로 확정하지 않는다. 성공한 시제품을 제품 EEPROM 완성으로 세지 않는다.
