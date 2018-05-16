/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief Valuepair functions that are radiusd-specific and as such do not
 * 	  belong in the library.
 * @file main/pair.c
 *
 * @ingroup AVP
 *
 * @copyright 2000,2006  The FreeRADIUS server project
 * @copyright 2000  Alan DeKok <aland@ox.org>
 */

RCSID("$Id$")

#include <ctype.h>

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/rad_assert.h>

struct cmp {
	fr_dict_attr_t const	*attribute;
	fr_dict_attr_t const	*from;
	bool			first_only;
	void			*instance; /* module instance */
	RAD_COMPARE_FUNC	compare;
	struct cmp		*next;
};
static struct cmp *cmp;

/** Compares check and vp by value.
 *
 * Does not call any per-attribute comparison function, but does honour
 * check.operator. Basically does "vp.value check.op check.value".
 *
 * @param request Current request.
 * @param check rvalue, and operator.
 * @param vp lvalue.
 * @return
 *	- 0 if check and vp are equal
 *	- -1 if vp value is less than check value.
 *	- 1 is vp value is more than check value.
 *	- -2 on error.
 */
#ifdef HAVE_REGEX
int radius_compare_vps(REQUEST *request, VALUE_PAIR *check, VALUE_PAIR *vp)
#else
int radius_compare_vps(UNUSED REQUEST *request, VALUE_PAIR *check, VALUE_PAIR *vp)
#endif
{
	int ret = 0;

	/*
	 *      Check for =* and !* and return appropriately
	 */
	if (check->op == T_OP_CMP_TRUE)  return 0;
	if (check->op == T_OP_CMP_FALSE) return 1;

#ifdef HAVE_REGEX
	if ((check->op == T_OP_REG_EQ) || (check->op == T_OP_REG_NE)) {
		ssize_t		slen;
		regex_t		*preg = NULL;
		regmatch_t	rxmatch[REQUEST_MAX_REGEX + 1];	/* +1 for %{0} (whole match) capture group */
		size_t		nmatch = sizeof(rxmatch) / sizeof(regmatch_t);

		char *expr = NULL, *value = NULL;
		char const *expr_p, *value_p;

		if (check->vp_type == FR_TYPE_STRING) {
			expr_p = check->vp_strvalue;
		} else {
			expr_p = expr = fr_pair_value_asprint(check, check, '\0');
		}

		if (vp->vp_type == FR_TYPE_STRING) {
			value_p = vp->vp_strvalue;
		} else {
			value_p = value = fr_pair_value_asprint(vp, vp, '\0');
		}

		if (!expr_p || !value_p) {
			REDEBUG("Error stringifying operand for regular expression");

		regex_error:
			talloc_free(preg);
			talloc_free(expr);
			talloc_free(value);
			return -2;
		}

		/*
		 *	Include substring matches.
		 */
		slen = regex_compile(request, &preg, expr_p, talloc_array_length(expr_p) - 1, false, false, true, true);
		if (slen <= 0) {
			REMARKER(expr_p, -slen, fr_strerror());

			goto regex_error;
		}

		slen = regex_exec(preg, value_p, talloc_array_length(value_p) - 1, rxmatch, &nmatch);
		if (slen < 0) {
			RPERROR("Invalid regex");

			goto regex_error;
		}

		if (check->op == T_OP_REG_EQ) {
			/*
			 *	Add in %{0}. %{1}, etc.
			 */
			regex_sub_to_request(request, &preg, value_p, talloc_array_length(value_p) - 1,
					     rxmatch, nmatch);
			ret = (slen == 1) ? 0 : -1;
		} else {
			ret = (slen != 1) ? 0 : -1;
		}

		talloc_free(preg);
		talloc_free(expr);
		talloc_free(value);
		goto finish;
	}
#endif

	/*
	 *	Attributes must be of the same type.
	 *
	 *	FIXME: deal with type mismatch properly if one side contain
	 *	ABINARY, OCTETS or STRING by converting the other side to
	 *	a string
	 *
	 */
	if (vp->vp_type != check->vp_type) return -1;

	/*
	 *	Tagged attributes are equal if and only if both the
	 *	tag AND value match.
	 */
	if (check->da->flags.has_tag && !TAG_EQ(check->tag, vp->tag)) {
		ret = ((int) vp->tag) - ((int) check->tag);
		if (ret != 0) goto finish;
	}

	/*
	 *	Not a regular expression, compare the types.
	 */
	switch (check->vp_type) {
#ifdef WITH_ASCEND_BINARY
		/*
		 *	Ascend binary attributes can be treated
		 *	as opaque objects, I guess...
		 */
		case FR_TYPE_ABINARY:
#endif
		case FR_TYPE_OCTETS:
			if (vp->vp_length != check->vp_length) {
				ret = 1; /* NOT equal */
				break;
			}
			ret = memcmp(vp->vp_strvalue, check->vp_strvalue, vp->vp_length);
			break;

		case FR_TYPE_STRING:
			ret = strcmp(vp->vp_strvalue,
				     check->vp_strvalue);
			break;

		case FR_TYPE_UINT8:
			ret = vp->vp_uint8 - check->vp_uint8;
			break;
		case FR_TYPE_UINT16:
			ret = vp->vp_uint16 - check->vp_uint16;
			break;
		case FR_TYPE_UINT32:
			ret = vp->vp_uint32 - check->vp_uint32;
			break;

		case FR_TYPE_UINT64:
			/*
			 *	Don't want integer overflow!
			 */
			if (vp->vp_uint64 < check->vp_uint64) {
				ret = -1;
			} else if (vp->vp_uint64 > check->vp_uint64) {
				ret = +1;
			} else {
				ret = 0;
			}
			break;

		case FR_TYPE_INT32:
			if (vp->vp_int32 < check->vp_int32) {
				ret = -1;
			} else if (vp->vp_int32 > check->vp_int32) {
				ret = +1;
			} else {
				ret = 0;
			}
			break;

		case FR_TYPE_DATE:
			ret = vp->vp_date - check->vp_date;
			break;

		case FR_TYPE_IPV4_ADDR:
			ret = ntohl(vp->vp_ipv4addr) - ntohl(check->vp_ipv4addr);
			break;

		case FR_TYPE_IPV6_ADDR:
			ret = memcmp(vp->vp_ip.addr.v6.s6_addr, check->vp_ip.addr.v6.s6_addr,
				     sizeof(vp->vp_ip.addr.v6.s6_addr));
			break;

		case FR_TYPE_IPV4_PREFIX:
		case FR_TYPE_IPV6_PREFIX:
			ret = memcmp(&vp->vp_ip, &check->vp_ip, sizeof(vp->vp_ip));
			break;

		case FR_TYPE_IFID:
			ret = memcmp(vp->vp_ifid, check->vp_ifid, sizeof(vp->vp_ifid));
			break;

		default:
			break;
	}

finish:
	if (ret > 0) return 1;
	if (ret < 0) return -1;
	return 0;
}


/** Compare check and vp. May call the attribute comparison function.
 *
 * Unlike radius_compare_vps() this function will call any attribute-specific
 * comparison functions registered.
 *
 * @param request Current request.
 * @param req list pairs.
 * @param check item to compare.
 * @param check_pairs list.
 * @param reply_pairs list.
 * @return
 *	- 0 if check and vp are equal.
 *	- -1 if vp value is less than check value.
 *	- 1 is vp value is more than check value.
 */
int radius_callback_compare(REQUEST *request, VALUE_PAIR *req,
			    VALUE_PAIR *check, VALUE_PAIR *check_pairs,
			    VALUE_PAIR **reply_pairs)
{
	struct cmp *c;

	/*
	 *      Check for =* and !* and return appropriately
	 */
	if (check->op == T_OP_CMP_TRUE)  return 0;
	if (check->op == T_OP_CMP_FALSE) return 1;

	/*
	 *	See if there is a special compare function.
	 *
	 *	FIXME: use new RB-Tree code.
	 */
	for (c = cmp; c; c = c->next) {
		if (c->attribute == check->da) {
			return (c->compare)(c->instance, request, req, check,
				check_pairs, reply_pairs);
		}
	}

	if (!request) return -1; /* doesn't exist, don't compare it */

	return radius_compare_vps(request, check, req);
}


/** Find a comparison function for two attributes.
 *
 * @todo this should probably take DA's.
 * @param attribute to find comparison function for.
 * @return
 *	- true if a comparison function was found
 *	- false.
 */
int radius_find_compare(fr_dict_attr_t const *attribute)
{
	struct cmp *c;

	for (c = cmp; c; c = c->next) {
		if (c->attribute == attribute) {
			return true;
		}
	}

	return false;
}


/** See what attribute we want to compare with.
 *
 * @param attribute to find comparison function for.
 * @param from reference to compare with
 * @return
 *	- true if the comparison callback require a matching attribute in the request.
 *	- false.
 */
static bool otherattr(fr_dict_attr_t const *attribute, fr_dict_attr_t const **from)
{
	struct cmp *c;

	for (c = cmp; c; c = c->next) {
		if (c->attribute == attribute) {
			*from = c->from;
			return c->first_only;
		}
	}

	*from = attribute;
	return false;
}

/** Register a function as compare function
 *
 * @param name the attribute comparison to register
 * @param from the attribute we want to compare with. Normally this is the same as attribute.
 *	If null call the comparison function on every attributes in the request if first_only is
 *	false.
 * @param first_only will decide if we loop over the request attributes or stop on the first one.
 * @param func comparison function.
 * @param instance argument to comparison function.
 * @return
 *	- 0 on success
 *	- <0 on error
 */
int paircompare_register_byname(char const *name, fr_dict_attr_t const *from,
				bool first_only, RAD_COMPARE_FUNC func, void *instance)
{
	fr_dict_attr_flags_t	flags = {
					.compare = 1
				};

	fr_dict_attr_t const	*da;

	da = fr_dict_attr_by_name(fr_dict_internal, name);
	if (da) {
		if (!da->flags.compare) {
			fr_strerror_printf("Attribute '%s' already exists", name);
			return -1;
		}
	} else if (from) {
		if (fr_dict_attr_add(fr_dict_internal, fr_dict_root(fr_dict_internal),
				     name, -1, from->type, &flags) < 0) {
			fr_strerror_printf_push("Failed creating attribute '%s'", name);
			return -1;
		}

		da = fr_dict_attr_by_name(fr_dict_internal, name);
		if (!da) {
			fr_strerror_printf("Failed finding attribute '%s'", name);
			return -1;
		}

		DEBUG("Creating attribute %s", name);
	}

	return paircompare_register(da, from, first_only, func, instance);
}

/** Register a function as compare function.
 *
 * @param attribute to register comparison function for.
 * @param from the attribute we want to compare with. Normally this is the same as attribute.
 *	If null call the comparison function on every attributes in the request if first_only is
 *	false.
 * @param first_only will decide if we loop over the request attributes or stop on the first one.
 * @param func comparison function.
 * @param instance argument to comparison function.
 * @return 0
 */
int paircompare_register(fr_dict_attr_t const *attribute, fr_dict_attr_t const *from,
			 bool first_only, RAD_COMPARE_FUNC func, void *instance)
{
	struct cmp *c;

	rad_assert(attribute != NULL);

	paircompare_unregister(attribute, func);

	MEM(c = talloc_zero(NULL, struct cmp));

	c->compare   = func;
	c->attribute = attribute;
	c->from = from;
	c->first_only = first_only;
	c->instance  = instance;
	c->next      = cmp;
	cmp = c;

	return 0;
}

/** Unregister comparison function for an attribute
 *
 * @param attribute dict reference to unregister for.
 * @param func comparison function to remove.
 */
void paircompare_unregister(fr_dict_attr_t const *attribute, RAD_COMPARE_FUNC func)
{
	struct cmp *c, *last;

	last = NULL;
	for (c = cmp; c; c = c->next) {
		if (c->attribute == attribute && c->compare == func) {
			break;
		}
		last = c;
	}

	if (c == NULL) return;

	if (last != NULL) {
		last->next = c->next;
	} else {
		cmp = c->next;
	}

	talloc_free(c);
}

/** Unregister comparison function for a module
 *
 *  All paircompare() functions for this module will be unregistered.
 *
 * @param instance the module instance
 */
void paircompare_unregister_instance(void *instance)
{
	struct cmp *c, **tail;

	tail = &cmp;
	while ((c = *tail) != NULL) {
		if (c->instance == instance) {
			*tail = c->next;
			talloc_free(c);
			continue;
		}

		tail = &(c->next);
	}
}

/** Compare two pair lists except for the password information.
 *
 * For every element in "check" at least one matching copy must be present
 * in "reply".
 *
 * @param[in] request Current request.
 * @param[in] req_list request valuepairs.
 * @param[in] check Check/control valuepairs.
 * @param[in,out] rep_list Reply value pairs.
 *
 * @return 0 on match.
 */
int paircompare(REQUEST *request, VALUE_PAIR *req_list, VALUE_PAIR *check,
		VALUE_PAIR **rep_list)
{
	fr_cursor_t		cursor;
	VALUE_PAIR		*check_item;
	VALUE_PAIR		*auth_item;
	fr_dict_attr_t const	*from;

	int			result = 0;
	int			compare;
	bool			first_only;

	for (check_item = fr_cursor_init(&cursor, &check);
	     check_item;
	     check_item = fr_cursor_next(&cursor)) {
		/*
		 *	If the user is setting a configuration value,
		 *	then don't bother comparing it to any attributes
		 *	sent to us by the user.  It ALWAYS matches.
		 */
		if ((check_item->op == T_OP_SET) ||
		    (check_item->op == T_OP_ADD)) {
			continue;
		}

		if (fr_dict_attr_is_top_level(check_item->da)) switch (check_item->da->attr) {
		/*
		 *	Attributes we skip during comparison.
		 *	These are "server" check items.
		 */
		case FR_CRYPT_PASSWORD:
		case FR_AUTH_TYPE:
		case FR_AUTZ_TYPE:
		case FR_ACCT_TYPE:
		case FR_SESSION_TYPE:
		case FR_STRIP_USER_NAME:
			continue;

		/*
		 *	IF the password attribute exists, THEN
		 *	we can do comparisons against it.  If not,
		 *	then the request did NOT contain a
		 *	User-Password attribute, so we CANNOT do
		 *	comparisons against it.
		 *
		 *	This hack makes CHAP-Password work..
		 */
		case FR_USER_PASSWORD:
			if (check_item->op == T_OP_CMP_EQ) {
				WARN("Found User-Password == \"...\"");
				WARN("Are you sure you don't mean Cleartext-Password?");
				WARN("See \"man rlm_pap\" for more information");
			}
			if (fr_pair_find_by_num(req_list, 0, FR_USER_PASSWORD, TAG_ANY) == NULL) {
				continue;
			}
			break;
		}

		/*
		 *	See if this item is present in the request.
		 */
		first_only = otherattr(check_item->da, &from);

		auth_item = req_list;
	try_again:
		if (!first_only) {
			while (auth_item != NULL) {
				if ((auth_item->da == from) || (!from)) {
					break;
				}
				auth_item = auth_item->next;
			}
		}

		/*
		 *	Not found, it's not a match.
		 */
		if (auth_item == NULL) {
			/*
			 *	Didn't find it.  If we were *trying*
			 *	to not find it, then we succeeded.
			 */
			if (check_item->op == T_OP_CMP_FALSE) {
				continue;
			} else {
				return -1;
			}
		}

		/*
		 *	Else we found it, but we were trying to not
		 *	find it, so we failed.
		 */
		if (check_item->op == T_OP_CMP_FALSE) {
			return -1;
		}

		/*
		 *	We've got to xlat the string before doing
		 *	the comparison.
		 */
		xlat_eval_pair(request, check_item);

		/*
		 *	OK it is present now compare them.
		 */
		compare = radius_callback_compare(request, auth_item,
						  check_item, check, rep_list);

		switch (check_item->op) {
		case T_OP_EQ:
		default:
			RWDEBUG("Invalid operator '%s' for item %s: reverting to '=='",
				fr_int2str(fr_tokens_table, check_item->op, "<INVALID>"), check_item->da->name);
			/* FALL-THROUGH */
		case T_OP_CMP_TRUE:
		case T_OP_CMP_FALSE:
		case T_OP_CMP_EQ:
			if (compare != 0) result = -1;
			break;

		case T_OP_NE:
			if (compare == 0) result = -1;
			break;

		case T_OP_LT:
			if (compare >= 0) result = -1;
			break;

		case T_OP_GT:
			if (compare <= 0) result = -1;
			break;

		case T_OP_LE:
			if (compare > 0) result = -1;
			break;

		case T_OP_GE:
			if (compare < 0) result = -1;
			break;

#ifdef HAVE_REGEX
		case T_OP_REG_EQ:
		case T_OP_REG_NE:
			if (compare != 0) result = -1;
			break;
#endif
		} /* switch over the operator of the check item */

		/*
		 *	This attribute didn't match, but maybe there's
		 *	another of the same attribute, which DOES match.
		 */
		if ((result != 0) && (!first_only)) {
			auth_item = auth_item->next;
			result = 0;
			goto try_again;
		}

	} /* for every entry in the check item list */

	return result;
}