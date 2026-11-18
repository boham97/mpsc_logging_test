# 사용법: perf script -i <perf.data> -F tid,ip,sym --inline > out.txt && python3 tools/perf_classify.py out.txt
# 스레드별 샘플 비중과, 로깅 스레드(consumer_main 이 스택에 있는 스레드)의 호출 스택 분류를 찍는다.
import sys, collections
blocks=open(sys.argv[1]).read().split("\n\n")
by_tid=collections.Counter(); stacks=collections.defaultdict(list)
for b in blocks:
    ls=[l.strip() for l in b.strip().split("\n") if l.strip()]
    if not ls: continue
    tid=ls[0].split()[0]
    syms=[l.split(None,1)[1] if len(l.split(None,1))>1 else "?" for l in ls[1:]]
    by_tid[tid]+=1; stacks[tid].append(syms)
tot=sum(by_tid.values())
print("전체 샘플", tot)
for t,c in by_tid.most_common(): print(f"  tid {t}: {c} ({100*c/tot:.1f}%)")
# 로깅 스레드 = consumer_main 이 스택에 있는 스레드
cons=[t for t in by_tid if any(any("consumer_main" in s for s in st) for st in stacks[t][:200])]
for t in cons:
    st=stacks[t]; n=len(st)
    print(f"\n로깅 스레드 {t}: 샘플 {n}")
    def has(stk,*names): return any(any(nm in s for nm in names) for s in stk)
    cats=collections.Counter()
    for stk in st:
        leaf=stk[0] if stk else "?"
        if has(stk,"__printf_fp","__printf_fphex") or (has(stk,"vsnprintf","snprintf") and has(stk,"mpn_","hack_digit","__printf_fp_l")):
            cats["%g: snprintf 안 부동소수점 변환 (__printf_fp 등)"]+=1
        elif has(stk,"out_printf","vsnprintf","snprintf"):
            cats["그 밖의 snprintf/vsnprintf (정수 %zu, 공통 처리)"]+=1
        elif has(stk,"log_format_prefix"):
            cats["접두어 직접 변환"]+=1
        elif has(stk,"render_message","parse_spec","next_arg","strchr"):
            cats["메시지 해석 (render_message, parse_spec, strchr 등)"]+=1
        elif has(stk,"write","writev","flush"):
            cats["파일 쓰기"]+=1
        else:
            cats[f"그 밖: {leaf}"]+=1
    for k,c in cats.most_common(15): print(f"  {100*c/n:5.1f}%  {k}")
    leaf=collections.Counter(stk[0] for stk in st if stk)
    print("  -- 직접 쓴 시간(잎) 상위")
    for k,c in leaf.most_common(15): print(f"  {100*c/n:5.1f}%  {k}")
    fp=sum(1 for stk in st if has(stk,"__printf_fp"))
    print(f"  == 스택에 __printf_fp 가 있는 샘플: {100*fp/n:.1f}%")
    sn=sum(1 for stk in st if has(stk,"snprintf"))
    print(f"  == 스택에 snprintf/vsnprintf 가 있는 샘플: {100*sn/n:.1f}%")
