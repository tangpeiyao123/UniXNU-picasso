/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * picasso SCM 只读探针 —— 判断 TrustZone 是否允许 AP 调用 PAS 服务。
 *
 * 为什么需要这个探针
 * ------------------
 * 项目现状：内核里 `scm_call` / `qcom_scm` / `pas_init` 全 0 次，设备树里没有
 * `qcom,scm` 节点，`NativeWlan` 自己也写着 "no SCM/TZ call in this tree"。
 * 而 ADSP（pas 0x1）/ modem（pas 0x4）的固件**必须**过 TrustZone 鉴权才能启动。
 *
 * 唯一已知的正向证据：内核里有 56 处 `smc #0`（PSCI 用它做 CPU 上下电，多核在跑），
 * 说明 **SMC 指令通路是活的**。但这不能证明 TZ 允许调 PAS —— 那取决于 XBL 里的
 * SMC 白名单，没有任何公开文档能回答。
 *
 * 所以先花最小的代价问一次：只做两个**无副作用**的查询，
 *   · GET_VERSION        —— SCM 通路本身是否可用
 *   · IS_CALL_AVAILABLE  —— PAS 的四个命令是否在白名单里
 * 两者都不改任何远端状态；失败只返回错误码。
 *
 * ⚠️ 不要在这里调用 PAS_INIT_IMAGE / PAS_AUTH_AND_RESET —— 那会真的动远端电源域。
 *
 * 编码来源（严格区分，不混用）
 * ---------------------------
 * [参考实现] `HeliumStudio-Dev/hackios-xnu-kernel` 的 `xnu/files/bsd/dev/hakios.mpss.c:1278-1280`：
 *      #define SCM_64  (1ull << 30)
 *      #define SCM_SIP (2ull << 24)
 *      #define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
 *   以及同文件 :1298-1330 的 `scm()`：x0..x7 + "smc #0" + 被中断时重试。
 *   这份实现被用于 K20 Pro（SM8150）真实启动 modem，是经过实测的调用形态。
 *
 * [Linux] `include/linux/qcom_scm.h` 的服务/命令号（公开头文件）：
 *      QCOM_SCM_SVC_INFO = 0x1, QCOM_SCM_INFO_GET_IS_CALL_AVAILABLE = 0x3
 *      QCOM_SCM_SVC_PIL  = 0x2, INIT_IMAGE=0x1 / MEM_SETUP=0x2 / AUTH_AND_RESET=0x5 / SHUTDOWN=0x6
 *      QCOM_SCM_SVC_MP   = 0xc, ASSIGN_MEM_REQ=0x16
 *
 * 注意 GET_VERSION 的服务/命令号在不同版本里出现过两种写法（0x1/0x1 与 0x1/0x0），
 * 所以探针会把两种都试一次，让实测结果说话，而不是先验假设。
 */

#include <stdint.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>


/* ---- 与参考实现一致的编码 ---- */
#define PICASSO_SCM_64          (1ull << 30)
#define PICASSO_SCM_SIP         (2ull << 24)
#define PICASSO_SCM_CALL(svc, cmd) ((uint64_t)(svc) << 8 | (uint64_t)(cmd))

/* ---- Linux qcom_scm.h 的服务号 ---- */
#define PICASSO_SCM_SVC_INFO    0x1u
#define PICASSO_SCM_SVC_PIL     0x2u
#define PICASSO_SCM_SVC_MP      0xcu

/* ---- 探针要查的命令 ---- */
#define PICASSO_SCM_INFO_VERSION          0x1u
#define PICASSO_SCM_INFO_IS_CALL_AVAIL    0x3u
#define PICASSO_SCM_PIL_INIT_IMAGE        0x1u
#define PICASSO_SCM_PIL_MEM_SETUP         0x2u
#define PICASSO_SCM_PIL_AUTH_AND_RESET    0x5u
#define PICASSO_SCM_PIL_SHUTDOWN          0x6u
#define PICASSO_SCM_MP_ASSIGN             0x16u

#define PICASSO_SCM_INTERRUPTED 1ull
#define PICASSO_SCM_RETRIES     100

/*
 * arginfo（x1）的两种可能编码。
 *
 * [参考实现依据] hakios.mpss.c:1285-1287 + 3564：
 *      SCM_ONE_VALUE 1 / SCM_THREE_VALUES 3 / SCM_VALUE_AND_BUFFER (2 | 2<<6)
 *      SCM_ASSIGN_ARGUMENTS 0x1117   -- 注释写 "seven: a buffer, a value, ..."
 * 三个常量互相对照可以看出：**参数个数在 bit 0-3，参数类型从 bit 4 开始每 2 位一个**。
 *   0x1117 的低 4 位 = 7，与注释的 "seven" 吻合。
 * 这与 Linux qcom_scm.h 的 QCOM_SCM_ARGS 宏布局一致，所以 QCOM_SCM_ARGS(1) == 1。
 *
 * 但参考实现本身也是从 Linux 移植的，不能当作"这台设备 TZ 一定认"的证明，
 * 因此探针把两种都试一次：
 *   PICASSO_ARGINFO_PLAIN  = 1     (1 个参数，类型位全 0)
 *   PICASSO_ARGINFO_VAL    = 0x11  (1 个参数，类型 VAL=1)
 * 全是只读查询，多试一种没有副作用。
 */
#define PICASSO_ARGINFO_PLAIN   1ull
#define PICASSO_ARGINFO_VAL     0x11ull

/* 探针累计调用次数，便于在长日志里确认只跑过一轮 */
volatile uint32_t picasso_scm_probe_calls;
volatile int32_t  picasso_scm_version_status = -999;
volatile uint64_t picasso_scm_version_value;
volatile uint32_t picasso_scm_pas_available_count;

/*
 * 一次 SMC 调用。寄存器布局与重试逻辑照搬参考实现：
 *   x0 = function id（首次）或 SCM_INTERRUPTED（重试，表示"继续上次被中断的调用"）
 *   x1 = 参数个数/形态, x2..x5 = 参数, x6 = session(返回), x7 = 0
 * 返回 x0（int32），answer 取 x1。
 *
 * 没有加锁：本探针由单一内核线程在启动阶段调用一次，不并发。
 */
static int32_t
picasso_scm_call(const char *what, uint64_t function, uint64_t arguments,
    uint64_t first, uint64_t second, uint64_t third, uint64_t fourth,
    uint64_t *answer)
{
	uint64_t session = 0;
	const uint64_t fnid = function | PICASSO_SCM_64 | PICASSO_SCM_SIP;
	uint64_t last_x0 = 0, last_x1 = 0, last_x6 = 0;

	for (int attempt = 0; attempt < PICASSO_SCM_RETRIES; attempt++) {
		register uint64_t x0 __asm__("x0") = attempt == 0 ? fnid : PICASSO_SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = fourth;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0"
		    : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3),
		      "+r" (x4), "+r" (x5), "+r" (x6), "+r" (x7)
		    :
		    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
		      "x16", "x17", "memory");

		session = x6;
		last_x0 = x0;
		last_x1 = x1;
		last_x6 = x6;
		if (x0 != PICASSO_SCM_INTERRUPTED) {
			if (answer) {
				*answer = x1;
			}
			__atomic_add_fetch(&picasso_scm_probe_calls, 1, __ATOMIC_RELAXED);
			return (int32_t)x0;
		}
	}
	/*
	 * 重试用尽时返回**原始 x0**，不再合成 -1。
	 *
	 * 合成 -1 会把"TZ 一直回 1"伪装成"我们放弃了"，两者在日志上无法区分——
	 * 上一轮就是因此把 22 次调用误读成"SCM 全灭"，进而错判音频/WiFi 不可做。
	 * 参考实现（hakios.mpss.c:1278-1345）里 SCM_INTERRUPTED 同样是 1，
	 * 且它成功启动过 K20 Pro 的 modem，说明那台机器上成功码是 0。
	 * 所以这里只如实上报，不代替硬件下结论。
	 */
	if (answer) {
		*answer = last_x1;
	}
	__atomic_add_fetch(&picasso_scm_probe_calls, 1, __ATOMIC_RELAXED);
	IOLog("PICASSO_SCM %s: x0 stayed %#llx for %d tries (x1=%#llx x6=%#llx), reporting raw\n",
	    what, (unsigned long long)last_x0, PICASSO_SCM_RETRIES,
	    (unsigned long long)last_x1, (unsigned long long)last_x6);
	return (int32_t)last_x0;
}

/*
 * 单次 SMC，不做重试、不做任何解释，把返回的全部寄存器原样打出来。
 *
 * 之所以需要它：picasso_scm_call 把 x0==1 当成"被中断"并重试 100 次，
 * 最后只报 -1，x1..x6 到底是什么全被吞掉了。而 x0==1 在这块 TZ 上未必是
 * "忙"——也可能就是成功码。看不到原始返回，就无法区分"TZ 锁死"和"我们读错"。
 *
 * 另外加一条 PSCI 对照（fnid 0x84000000 = PSCI_VERSION）：这台机器的 8 个 CPU
 * 都是 PSCI 拉起来的，它必然可用。如果连它都返回垃圾，说明内联汇编本身有问题，
 * 而不是 TZ 拒绝 SCM。
 */
static void
picasso_smc_raw(const char *what, uint64_t fnid, uint64_t arguments)
{
	register uint64_t x0 __asm__("x0") = fnid;
	register uint64_t x1 __asm__("x1") = arguments;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	register uint64_t x4 __asm__("x4") = 0;
	register uint64_t x5 __asm__("x5") = 0;
	register uint64_t x6 __asm__("x6") = 0;
	register uint64_t x7 __asm__("x7") = 0;

	__asm__ volatile ("smc #0"
	    : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3),
	      "+r" (x4), "+r" (x5), "+r" (x6), "+r" (x7)
	    :
	    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
	      "x16", "x17", "memory");

	IOLog("PICASSO_SCM raw %s fnid=%#llx arg=%#llx -> x0=%#llx x1=%#llx x2=%#llx x3=%#llx x6=%#llx\n",
	    what, (unsigned long long)fnid, (unsigned long long)arguments,
	    (unsigned long long)x0, (unsigned long long)x1, (unsigned long long)x2,
	    (unsigned long long)x3, (unsigned long long)x6);
}

/*
 * IS_CALL_AVAILABLE 的两种入参形态都试一次。
 *
 * Linux 的 __qcom_scm_is_call_available() 传 1 个值：把 (svc, cmd) 打包进 args[0]。
 * 但打包方式在历史上有过 service<<8|cmd 与 svc|cmd<<8 两种，且是否带 SCM_SIP/SCM_64
 * 标志位也不确定 —— 所以这里两种都发一次，用返回值判断哪种被接受。
 * 全是只读查询，多试一次没有副作用。
 */
static void
picasso_scm_is_call_available(const char *label, uint32_t svc, uint32_t cmd)
{
	/* 二维：arginfo 两种编码 × args[0] 两种打包。任一组合被接受即认为该命令可用。 */
	static const struct {
		uint64_t arginfo;
		const char *name;
	} infos[] = {
		{ PICASSO_ARGINFO_PLAIN, "arginfo=1" },
		{ PICASSO_ARGINFO_VAL,   "arginfo=0x11" },
	};
	static const struct {
		uint64_t pack;
		const char *name;
	} packs[] = {
		{ 0, "pack=svc<<8|cmd" },
		{ PICASSO_SCM_64 | PICASSO_SCM_SIP, "pack=+SIP|64" },
	};

	const uint64_t base = PICASSO_SCM_CALL(svc, cmd);
	const uint64_t fnid = PICASSO_SCM_CALL(PICASSO_SCM_SVC_INFO, PICASSO_SCM_INFO_IS_CALL_AVAIL);
	unsigned accepted = 0;

	const size_t ninfos = sizeof(infos) / sizeof(infos[0]);
	const size_t npacks = sizeof(packs) / sizeof(packs[0]);

	for (size_t i = 0; i < ninfos; i++) {
		for (size_t k = 0; k < npacks; k++) {
			const uint64_t arg0 = base | packs[k].pack;
			uint64_t answer = 0;
			const int32_t status = picasso_scm_call("IS_CALL_AVAILABLE",
			    fnid, infos[i].arginfo, arg0, 0, 0, 0, &answer);
			IOLog("PICASSO_SCM iscall %s %s %s arg=%#llx -> status=%d answer=%#llx\n",
			    label, infos[i].name, packs[k].name,
			    (unsigned long long)arg0, status, (unsigned long long)answer);
			if (status == 0 && answer != 0) {
				accepted = 1;
			}
		}
	}

	if (accepted) {
		__atomic_add_fetch(&picasso_scm_pas_available_count, 1u, __ATOMIC_RELAXED);
	}
}

/*
 * 探针主体。由 boot-arg `sheng-scm=1` 触发（默认关闭，避免影响正常启动）。
 *
 * 输出全部以 PICASSO_SCM 开头，便于在控制台日志里 grep。
 */
void
picasso_scm_probe(void)
{
	int enabled = 0;
	if (!PE_parse_boot_argn("sheng-scm", &enabled, sizeof(enabled)) || enabled != 1) {
		return;
	}

	int32_t status;
	uint64_t answer = 0;

	IOLog("PICASSO_SCM probe begin (read-only: GET_VERSION + IS_CALL_AVAILABLE only)\n");

	/*
	 * --- 0. 原始返回与控制组 ---
	 *
	 * PSCI_VERSION 是控制组：它必然可用（CPU 就是它拉起来的）。如果它返回
	 * 0x00010001 这类正常版本号，说明内联汇编与寄存器传参是对的，那么 SCM
	 * 返回 1 就是 TZ 的真实回应；如果它也返回垃圾，问题在我们自己的代码。
	 */
	picasso_smc_raw("PSCI_VERSION", 0x84000000ull, 0);
	picasso_smc_raw("SCM_GET_VERSION(1,1)+flags",
	    PICASSO_SCM_CALL(PICASSO_SCM_SVC_INFO, PICASSO_SCM_INFO_VERSION)
	        | PICASSO_SCM_64 | PICASSO_SCM_SIP, 0);
	picasso_smc_raw("SCM_GET_VERSION(1,1)no-flags",
	    PICASSO_SCM_CALL(PICASSO_SCM_SVC_INFO, PICASSO_SCM_INFO_VERSION), 0);

	/* --- 1. GET_VERSION：两种常见的 (svc, cmd) 组合各试一次 --- */
	status = picasso_scm_call("GET_VERSION(1,1)",
	    PICASSO_SCM_CALL(PICASSO_SCM_SVC_INFO, PICASSO_SCM_INFO_VERSION),
	    0, 0, 0, 0, 0, &answer);
	IOLog("PICASSO_SCM version svc=1 cmd=1 -> status=%d answer=%#llx\n",
	    status, (unsigned long long)answer);
	if (status == 0) {
		__atomic_store_n(&picasso_scm_version_status, status, __ATOMIC_RELAXED);
		__atomic_store_n(&picasso_scm_version_value, answer, __ATOMIC_RELAXED);
	}

	if (status != 0) {
		uint64_t alt = 0;
		int32_t st2 = picasso_scm_call("GET_VERSION(1,0)",
		    PICASSO_SCM_CALL(PICASSO_SCM_SVC_INFO, 0x0u),
		    0, 0, 0, 0, 0, &alt);
		IOLog("PICASSO_SCM version svc=1 cmd=0 -> status=%d answer=%#llx\n",
		    st2, (unsigned long long)alt);
		if (st2 == 0) {
			__atomic_store_n(&picasso_scm_version_status, st2, __ATOMIC_RELAXED);
			__atomic_store_n(&picasso_scm_version_value, alt, __ATOMIC_RELAXED);
		}
		status = st2 == 0 ? st2 : status;
	}

	if (status != 0) {
		/*
		 * GET_VERSION 不通有两种可能：TZ 完全不接受 SIP 调用，或者只是这个服务号不对。
		 * 继续查 IS_CALL_AVAILABLE —— 它对服务号更宽容（本身就是"查这个号在不在""）。
		 */
		IOLog("PICASSO_SCM version unavailable (status=%d); continuing with IS_CALL_AVAILABLE\n",
		    status);
	}

	/* --- 2. PAS 的四个命令是否在白名单里 --- */
	picasso_scm_is_call_available("PIL_INIT_IMAGE",      PICASSO_SCM_SVC_PIL, PICASSO_SCM_PIL_INIT_IMAGE);
	picasso_scm_is_call_available("PIL_MEM_SETUP",       PICASSO_SCM_SVC_PIL, PICASSO_SCM_PIL_MEM_SETUP);
	picasso_scm_is_call_available("PIL_AUTH_AND_RESET", PICASSO_SCM_SVC_PIL, PICASSO_SCM_PIL_AUTH_AND_RESET);
	picasso_scm_is_call_available("PIL_SHUTDOWN",        PICASSO_SCM_SVC_PIL, PICASSO_SCM_PIL_SHUTDOWN);

	/* --- 3. 内存共享（PAS 之后必然要用，先看它在不在） --- */
	picasso_scm_is_call_available("MP_ASSIGN_MEM",       PICASSO_SCM_SVC_MP,  PICASSO_SCM_MP_ASSIGN);

	IOLog("PICASSO_SCM probe end calls=%u version_status=%d version=%#llx pas_available_count=%u\n",
	    (unsigned)__atomic_load_n(&picasso_scm_probe_calls, __ATOMIC_RELAXED),
	    (int)__atomic_load_n(&picasso_scm_version_status, __ATOMIC_RELAXED),
	    (unsigned long long)__atomic_load_n(&picasso_scm_version_value, __ATOMIC_RELAXED),
	    (unsigned)__atomic_load_n(&picasso_scm_pas_available_count, __ATOMIC_RELAXED));

	/*
	 * 判读方式（写在这里，避免事后凭印象解读）：
	 *   version_status == 0                 → SCM 通路可用，TZ 接受 SIP 调用
	 *   pas_available_count >= 1            → 至少一个 PAS 命令在白名单里，平台基座有路
	 *   version_status != 0 且 count == 0    → TZ 不可用或 PAS 被锁 → 音频/WiFi 需换路线
	 *   全部 call 都返回 -1（INTERRUPTED）   → SMC 指令在这个上下文不可用，换执行时机再试
	 */
}

