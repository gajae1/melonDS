# 빌드·배포·표준 기능·검증 기반 분석

기준: 1.1.02 소스 분석. 아래는 재현·측정·구현할 후보와 검증 계약이다. 정적 의심을 확인된 결함으로 읽지 않는다. 원래 관찰은 보존하며 현재 처리 상태는 [과제 목록](../Task_Catalog.md), 실제 변경은 [릴리스 기록](../releases/1.1.03.md)을 따른다.

| ID | 근거·트리거 | 최소 작업 단위 | 선행 조건 | 검증·완료 조건 | 수준/우선순위 |
|---|---|---|---|---|---|
| <a id="BV-01"></a>BV-01 | 과거 문서의 C11/미빌드/승인 대기가 현재 상태와 섞임 | baseline·실패·skip·장치 목록·작업 ID가 연결되는 문서/증거 index | 1.1.02 기록 | 기록마다 source revision·환경·검사 범위·남은 gate 식별 | 관찰/P1 |
| <a id="BV-02"></a>BV-02 | 루트 CMake는 patch<10만 0을 붙이고 package regex는 2자리 이상 허용 | 표시·숫자 버전 규칙 및 정렬, EXE/launcher/ZIP/resource의 단일 버전 입력 | 배포 profile | 1.1.09→10→99→100, 표시 1.1.0191↔숫자 patch191의 규칙·파일명 일치 | 확장/P1 |
| <a id="BV-03"></a>BV-03 | msys-dist.sh의 이름/경로 가정과 별도 package 도구 | 실제 EXE/Qt plugin import closure를 수집한 로컬 배포 명령 | 기존 도구체인 | clean PATH 실행, 미해결 import 0, 불필요 DLL 감소는 지원 기능 동일 조건에서만 채택 | 관찰/P1 |
| <a id="BV-04"></a>BV-04 | CI 전용 스크립트가 runner·설치·외부 작업에 의존 | 기존 CMake 명령을 재사용하는 로컬 build/validation profile | 설치된 SDK | 설치된 도구로 지정 build dir만 변경; 오염 없는 증거 출력 | 관찰/P1 |
| <a id="BV-05"></a>BV-05 | runtime 전체 순회에서 확장자/portable 경로로 개인 파일을 제외 | 배포 산출물 allowlist와 사용자 추가 파일 경고, staging 명세 | BV-03 | 합성 ROM/BIOS/설정/임의 사용자 파일이 ZIP에 없고 runtime 원본 불변 | 정적 위험/P1 |
| <a id="BV-06"></a>BV-06 | C23/C++26 모드와 실제 라이브러리 기능 지원은 별개 | 필요한 feature-test와 compile probe·호환 matrix; 의미 있는 `<bit>`/checked arithmetic/span 범위부터 | 기존 회귀 | 최소 GCC/Clang+각 STL 조합의 해당 기능 compile·동작; 임의 reflection/modules 도입 안 함 | 확장/P2 |
| <a id="BV-07"></a>BV-07 | DependencyAudit의 pin·요구 버전·실제 링크 버전이 다른 시점 | 설치/링크 graph·로컬 patch·원본 revision·라이선스 기록, 한 라이브러리씩 교체 | BV-01/03 | 해당 기능·패키지 실행 확인, vendored 차이 보존, 롤백 가능한 pin | 관찰/P1 |
| <a id="BV-08"></a>BV-08 | GCC15+ LTO 강제 OFF, blanket 해제 근거 없음 | 같은 소스/컴파일러에서 ICE 재현·최소화, 성공하는 조합에만 LTO/ThinLTO profile | BV-04/07 | compiler crash 없음, 정확성 동일, build시간/메모리/최종 frame time 비교 | 아이디어/P2 |
| <a id="BV-09"></a>BV-09 | PGO는 현재 명시적 수집/사용 도구가 없음 | 대표 workload 수집·profile 병합·별도 사용 빌드, holdout 게임/합성 workload | BV-01/04 | source/compiler 일치, stale profile 거절, 학습 밖 workload에서 회귀가 허용 예산 이내 | 아이디어/P2 |
| <a id="BV-10"></a>BV-10 | kernel benchmark와 전체 frame/장치 지연이 다른 값 | warm/cold·median/p95/p99·CPU/GPU/upload/readback/audio 대기를 나누는 기존 도구 확장 | 해당 경로 계측 | 같은 입력·출력의 paired 결과와 dispersion 보고, 전체 FPS로 과대 환산 안 함 | 확장/P1 |
| <a id="BV-11"></a>BV-11 | Sanitizers는 JIT fault handling·플랫폼 런타임과 충돌 가능 | parser/메모리·코어·JIT fault 경로를 구분한 ASan/UBSan/TSan 사용표 | BV-04 | 합성 트리거로 오류 검출, intentional fault 때문에 전체 검사를 꺼서 통과 처리하지 않음 | 확장/P2 |
| <a id="BV-12"></a>BV-12 | 다른 CPU의 지원 ISA·클럭·전력이 다름 | 커널별 검출·강제 fallback·측정 profile, NEON/crypto와 standard SIMD 후보 비교 | ISA 지원 계약·해당 커널 | unsupported 명령 0, 같은 출력, 특정 CPU/작업량에서만 측정된 이득 기록 | 확장/P1 |
| <a id="BV-13"></a>BV-13 | `-s` release strip과 별도 symbol package 경로 부족 | opt-in 분리 디버그 심볼·revision 일치 crash 분석 자료 | BV-03 | 같은 EXE의 주소→함수 복원, 공유 로그에서 ROM/메모리/개인 경로 기본 제외 | 확장/P2 |
| <a id="BV-14"></a>BV-14 | 과거 BSD/Linux frontend 빌드는 새 source validation이 아님 | Linux X11/Wayland 및 Free/Open/NetBSD 플랫폼별 실행 profile | SDK/기기 | frontend 실행·입력·소리·GPU/창 종료 확인, 각각 결과 구분 | 장치 대기/P2 |
| <a id="BV-15"></a>BV-15 | ARM64 emitter 모델은 실제 native 실행이 아님 | Windows/Linux ARM64 native baseline·NEON/AES/CRC 조건별 실행 | ARM64 native 실행 환경 | interpreter/JIT·forced fallback·장치 경로 동등, CPU 이름만으로 feature 허용 안 함 | 장치 대기/P1 |
| <a id="BV-16"></a>BV-16 | Apple SME/SME2 모델 정보와 OS 노출은 별개 | macOS/Apple SDK 조건·정확한 sysctl·ABI·thread 상태·장치 테스트 계획 | 지원 SDK와 native 실행 검증 | NEON baseline 먼저, SME는 대상 workload·정확성·실익 모두 입증해야 opt-in; 미입증이면 연구 상태 유지 | 조건부/P2 |
| <a id="BV-17"></a>BV-17 | 저장/기기/게임의 긴 세션 검증은 미완료 | feature별 장기 script와 수동 장치 시험, 게임 상태·소리·저장 비교 | 각 feature 안정 후보 | 실패/skip 구분, 실제 입력→화면/소리 측정, resume·장치 재연결·누수 추세 확인 | 확장/P1 |
| <a id="BV-18"></a>BV-18 | fork와 upstream의 수정 겹침·오래된 issue 제목 위험 | source revision 기준 patch provenance와 open/merged/중복/기각 대조 | BV-01 | 이미 해결된 결함 재적용 0; URL/commit과 실제 변경 경로 연결, 검증 실행 여부 명시 | 확장/P1 |
| <a id="BV-19"></a>BV-19 | 선택형 experimental 기능의 전체 구성 조합 증가 | 위험 기반 조합 matrix와 fallback·설정 저장·실패 복구·기본값 승격 절차 | Task_Catalog | device gate가 빈 기능은 stable 표시 금지; unsupported host에서 기존 기능 정상 | 확장/P1 |
| <a id="BV-20"></a>BV-20 | SDL2 API가 실제 Qt 오디오/컨트롤러/테스트에 연결됨 | SDL3 이행을 audio/gamepad/event/packaging 단위로 계획; 첫 단계는 별도 profile | 오디오·입력 기준선 | 기존 매핑/장치 복구/수명/출력 비교 후 의존성 전환. SDL2와 달라진 API를 단순 이름 치환하지 않음 | 신규/P2 |
