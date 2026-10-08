int f(struct ifbrparam *bparam)
{
	if (bparam->ifbrp_ctime 8
| <| 	    bparam->ifbrp_ctime > 3600)
		return (EINVAL);
	return (0);
}
