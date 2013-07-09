extern struct cpc_continuation *q_c_yield(struct cpc_continuation *cont);
struct cpc_continuation *qemu_coroutine_yield(struct cpc_continuation *cont)
{
	return q_c_yield(cont);
}
