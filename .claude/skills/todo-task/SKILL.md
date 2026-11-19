---
name: todo-task
description: logx 의 TODO.md 과제 하나를 원인 측정 → 변경 → 같은 세션 A/B 측정 → results 기록 → TODO/README 갱신 → 한국어 커밋까지 진행한다. "TODO n번 하자", "다음 과제", "이거 빨라졌는지 재 줘" 같은 요청에 쓴다.
---

# TODO 과제 진행 (logx)

이 저장소의 성능 작업은 항상 같은 순서로 한다. 지금까지 TODO 3·4번이 이 순서로 끝났다(커밋 `a836e30`, `51cfc8b`, `791731a`). 규칙의 자세한 근거는 `CLAUDE.md`의 "측정 규칙".

## 0. 고르기

- `TODO.md`를 읽고 대상 과제의 "현상 / 가설 / 확인 방법 / 완료 기준"을 확인한다.
- 근거로 적힌 `results/*.md`의 해석 절을 읽는다. 숫자는 거기서 가져온다.
- 사용자가 번호를 말하지 않았으면 TODO.md 가 정한 순서(지금은 1번 먼저)를 따르되, 고른 이유를 한 줄로 말한다.

## 1. 원인부터 잰다 (코드 바꾸기 전)

- 가설마다 숫자로 확인할 측정을 하나씩 정한다. 도구 선택:
  - 포맷팅 구간 비용: `fmt_bench`에 항목을 추가해서 잰다.
  - 스레드별 CPU 비중: `perf record -e cycles:u --call-graph dwarf <명령>` → `perf script | tools/perf_classify.py`
  - 시스템 콜·off-CPU: `tools/lf_offcpu.bt` (sudo, 사용자에게 실행 요청)
  - 캐시 라인 공유: `perf c2c`
  - ns 단위 핫패스 횟수: 스레드별 카운터를 넣은 계측 빌드 (uprobe 는 쓰지 않는다)
- 모든 측정은 `tools/save_result.sh <이름> <명령...>`으로 감싼다.
- 결과로 "상한이 어디인가"와 "고치면 기대치가 얼마인가"를 계산해 둔다. 예: `%g` 156ns → 포맷팅 상한 3.06 M줄/s ≈ 실제 처리량.
- 원인 확인만으로 의미 있으면 여기서 따로 커밋한다(예: `a836e30`).

## 2. 변경

- 직접 변환을 넣을 때는 결과가 `snprintf`와 바이트 단위로 같아야 한다. 확신 없는 입력은 `snprintf`로 넘긴다.
- `fmt_test`에 회귀 케이스를 넣는다. 수치 변환이면 경계값·동률·무작위 대량 1:1 비교.
- 테스트가 버그를 실제로 잡는지, 일부러 버그를 넣어 실패하는 것을 한 번 확인한다.
- `make` 경고 없음, `make test` 통과(결과는 자동으로 results 에 저장됨).

## 3. 같은 세션 A/B 측정

```sh
git worktree add <scratchpad>/old <변경 전 커밋>
make -C <scratchpad>/old cmp_bench
make cmp_bench
tools/save_result.sh <이름>_ab tools/cmp_ab.sh <scratchpad>/old/cmp_bench ./cmp_bench 5
git worktree remove <scratchpad>/old
```

- 처음이면 `cmp_ab.sh`가 `tools/ref_build.sh`로 Quill·spdlog 원본을 받는다(네트워크 필요, `build/ref/`).
- 다른 날·다른 세션의 숫자와 비교해서 결론 내지 않는다. 같은 라운드의 Quill 이 기준선이다.
- hot path p50 이 약 30ns / 약 100ns 사이에서 튀는 것은 머신 상태다(TODO 1). 라운드별로 표를 만들어 A·B 가 같이 움직였는지 본다.
- 포맷팅 비용이 바뀌었으면 `fmt_bench`도 다시 재서 저장한다.

## 4. 기록

새로 생긴 results 파일마다:
- "해석" 절의 "(작성 전)"을 채운다: 무엇을 비교했는지, 핵심 숫자 표(중앙값 [최소~최대]), 바뀌지 않은 것과 그 이유, 새로 보인 단서.
- `results/README.md` 목록에 한 줄 요약을 추가한다.

`TODO.md`:
- 표의 상태를 "완료 (MM-DD)"로 바꾸고, 해당 절에 **완료** 문단(근거 파일, 바꾼 것, 숫자)을 쓴다.
- 남은 일과 새로 발견한 것은 기존 과제에 덧붙이거나 새 번호로 넘긴다.

`README.md`:
- 해당 벤치마크 절의 숫자와 결과 링크를 고친다. 표를 다시 재지 않았으면 "위 표는 변경 전 값"이라고 적는다.
- "다음에 해볼 만한 것"에서 끝난 항목을 정리한다.

## 5. 커밋

- 결과 md, TODO.md, README.md, 코드를 한 커밋에 넣는다.
- 한국어 메시지. 제목: `<무엇을> 해서 <지표>를 <전> → <후> 로 올림 (TODO n)`. 본문은 `- ` 목록으로 변경점·검증·측정 숫자.
- 끝에 Co-Authored-By 줄.
- 커밋은 사용자가 요청했을 때 하고, 푸시는 따로 묻는다.

## 보고

사용자에게는 짧게: 원인(숫자), 바꾼 것, A/B 결과(같은 세션, Quill 대비), 남은 상한과 다음 과제 후보. 결과 파일 경로를 같이 준다.
