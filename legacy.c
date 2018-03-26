#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "objc/runtime.h"
#include "objc/encoding.h"
#include "ivar.h"
#include "properties.h"
#include "class.h"
#include "loader.h"

PRIVATE size_t lengthOfTypeEncoding(const char *types);

static ivar_ownership ownershipForIvar(struct legacy_gnustep_objc_class *cls, int idx)
{
	if (objc_get_class_version_legacy(cls) < 2)
	{
		return ownership_unsafe;
	}
	if (objc_bitfield_test(cls->strong_pointers, idx))
	{
		return ownership_strong;
	}
	if (objc_bitfield_test(cls->weak_pointers, idx))
	{
		return ownership_weak;
	}
	return ownership_unsafe;
}

static struct objc_ivar_list *upgradeIvarList(struct legacy_gnustep_objc_class *cls)
{
	struct objc_ivar_list_legacy *l = cls->ivars;
	if (l == NULL)
	{
		return NULL;
	}
	struct objc_ivar_list *n = calloc(1, sizeof(struct objc_ivar_list) +
			l->count*sizeof(struct objc_ivar));
	n->size = sizeof(struct objc_ivar);
	n->count = l->count;
	for (int i=0 ; i<l->count ; i++)
	{
		int nextOffset = (i+1 < l->count) ? l->ivar_list[i+1].offset : cls->instance_size;
		if (nextOffset < 0)
		{
			nextOffset = -nextOffset;
		}
		const char *type = l->ivar_list[i].type;
		int size = nextOffset - l->ivar_list[i].offset;
		n->ivar_list[i].name = l->ivar_list[i].name;
		n->ivar_list[i].type = type;
		if (objc_test_class_flag_legacy(cls, objc_class_flag_new_abi))
		{
			n->ivar_list[i].offset = cls->ivar_offsets[i];
		}
		else
		{
			n->ivar_list[i].offset = &l->ivar_list[i].offset;
		}
		n->ivar_list[i].align = ((type == NULL) || type[0] == 0) ? __alignof__(void*) : objc_alignof_type(type);
		if (type[0] == '\0')
		{
			n->ivar_list[i].align = size;
		}
		ivarSetOwnership(&n->ivar_list[i], ownershipForIvar(cls, i));
	}
	return n;
}

static struct objc_method_list *upgradeMethodList(struct objc_method_list_legacy *old)
{
	if (old == NULL)
	{
		return NULL;
	}
	struct objc_method_list *l = calloc(sizeof(struct objc_method_list) + old->count * sizeof(struct objc_method), 1);
	l->count = old->count;
	if (old->next)
	{
		l->next = upgradeMethodList(old->next);
	}
	l->size = sizeof(struct objc_method);
	for (int i=0 ; i<old->count ; i++)
	{
		l->methods[i].imp = old->methods[i].imp;
		l->methods[i].selector = old->methods[i].selector;
		l->methods[i].types = old->methods[i].types;
	}
	return l;
}

static inline BOOL checkAttribute(char field, int attr)
{
	return (field & attr) == attr;
}

static void upgradeProperty(struct objc_property *n, struct objc_property_gsv1 *o)
{
	size_t typeSize = lengthOfTypeEncoding(o->getter_types);
	char *typeEncoding = malloc(typeSize + 1);
	memcpy(typeEncoding, o->getter_types, typeSize);
	typeEncoding[typeSize] = 0;

	n->type = typeEncoding;
	if (o->getter_name)
	{
		n->getter = sel_registerTypedName_np(o->getter_name, o->getter_types);
	}
	if (o->setter_name)
	{
		n->setter = sel_registerTypedName_np(o->setter_name, o->setter_types);
	}

	if (o->name[0] == '\0')
	{
		n->name = o->name + o->name[1];
		n->attributes = o->name + 2;
		return;
	}

	n->name = o->name;

	const char *name = o->name;
	size_t nameSize = (NULL == name) ? 0 : strlen(name);
	// Encoding is T{type},V{name}, so 4 bytes for the "T,V" that we always
	// need.  We also need two bytes for the leading null and the length.
	size_t encodingSize = typeSize + nameSize + 6;
	char flags[20];
	size_t i = 0;
	// Flags that are a comma then a character
	if (checkAttribute(o->attributes, OBJC_PR_readonly))
	{
		flags[i++] = ',';
		flags[i++] = 'R';
	}
	if (checkAttribute(o->attributes, OBJC_PR_retain))
	{
		flags[i++] = ',';
		flags[i++] = '&';
	}
	if (checkAttribute(o->attributes, OBJC_PR_copy))
	{
		flags[i++] = ',';
		flags[i++] = 'C';
	}
	if (checkAttribute(o->attributes2, OBJC_PR_weak))
	{
		flags[i++] = ',';
		flags[i++] = 'W';
	}
	if (checkAttribute(o->attributes2, OBJC_PR_dynamic))
	{
		flags[i++] = ',';
		flags[i++] = 'D';
	}
	if ((o->attributes & OBJC_PR_nonatomic) == OBJC_PR_nonatomic)
	{
		flags[i++] = ',';
		flags[i++] = 'N';
	}
	encodingSize += i;
	flags[i] = '\0';
	size_t setterLength = 0;
	size_t getterLength = 0;
	if ((o->attributes & OBJC_PR_getter) == OBJC_PR_getter)
	{
		getterLength = strlen(o->getter_name);
		encodingSize += 2 + getterLength;
	}
	if ((o->attributes & OBJC_PR_setter) == OBJC_PR_setter)
	{
		setterLength = strlen(o->setter_name);
		encodingSize += 2 + setterLength;
	}
	unsigned char *encoding = malloc(encodingSize);
	// Set the leading 0 and the offset of the name
	unsigned char *insert = encoding;
	BOOL needsComma = NO;
	*(insert++) = 0;
	*(insert++) = 0;
	// Set the type encoding
	if (NULL != typeEncoding)
	{
		*(insert++) = 'T';
		memcpy(insert, typeEncoding, typeSize);
		insert += typeSize;
		needsComma = YES;
	}
	// Set the flags
	memcpy(insert, flags, i);
	insert += i;
	if ((o->attributes & OBJC_PR_getter) == OBJC_PR_getter)
	{
		if (needsComma)
		{
			*(insert++) = ',';
		}
		i++;
		needsComma = YES;
		*(insert++) = 'G';
		memcpy(insert, o->getter_name, getterLength);
		insert += getterLength;
	}
	if ((o->attributes & OBJC_PR_setter) == OBJC_PR_setter)
	{
		if (needsComma)
		{
			*(insert++) = ',';
		}
		i++;
		needsComma = YES;
		*(insert++) = 'S';
		memcpy(insert, o->setter_name, setterLength);
		insert += setterLength;
	}
	if (needsComma)
	{
		*(insert++) = ',';
	}
	*(insert++) = 'V';
	memcpy(insert, name, nameSize);
	insert += nameSize;
	*(insert++) = '\0';

	n->attributes = (const char*)encoding;
}

static struct objc_property_list *upgradePropertyList(struct objc_property_list_gsv1 *l)
{
	if (l == NULL)
	{
		return NULL;
	}
	size_t data_size = l->count * sizeof(struct objc_property);
	struct objc_property_list *n = calloc(1, sizeof(struct objc_property_list) + data_size);
	n->count = l->count;
	n->size = sizeof(struct objc_property);
	for (int i=0 ; i<l->count ; i++)
	{
		upgradeProperty(&n->properties[i], &l->properties[i]);
	}
	return n;
}

static int legacy_key;

PRIVATE struct legacy_gnustep_objc_class* objc_legacy_class_for_class(Class cls)
{
	return (struct legacy_gnustep_objc_class*)objc_getAssociatedObject((id)cls, &legacy_key);
}

PRIVATE Class objc_upgrade_class(struct legacy_gnustep_objc_class *oldClass)
{
	Class cls = calloc(sizeof(struct objc_class), 1);
	cls->isa = oldClass->isa;
	// super_class is left nil and we upgrade it later.
	cls->name = oldClass->name;
	cls->version = oldClass->version;
	cls->info = oldClass->info;
	cls->instance_size = oldClass->instance_size;
	cls->ivars = upgradeIvarList(oldClass);
	cls->methods = upgradeMethodList(oldClass->methods);
	cls->protocols = oldClass->protocols;
	cls->abi_version = oldClass->abi_version;
	cls->properties = upgradePropertyList(oldClass->properties);
	objc_register_selectors_from_class(cls);
	if (!objc_test_class_flag(cls, objc_class_flag_meta))
	{
		cls->isa = objc_upgrade_class((struct legacy_gnustep_objc_class*)cls->isa);
		objc_setAssociatedObject((id)cls, &legacy_key, (id)oldClass, OBJC_ASSOCIATION_ASSIGN);
	}
	return cls;
}

PRIVATE struct objc_category *objc_upgrade_category(struct objc_category_legacy *old)
{
	struct objc_category *cat = calloc(1, sizeof(struct objc_category));
	memcpy(cat, old, sizeof(struct objc_category_legacy));
	cat->instance_methods = upgradeMethodList(old->instance_methods);
	cat->class_methods = upgradeMethodList(old->class_methods);
	return cat;
}

PRIVATE struct objc_protocol *objc_upgrade_protocol_gcc(struct objc_protocol_gcc *p)
{
	// If the protocol has already been upgraded, the don't try to upgrade it twice.
	if (p->isa == objc_getClass("ProtocolGCC"))
	{
		return objc_getProtocol(p->name);
	}
	p->isa = objc_getClass("ProtocolGCC");
	Protocol *proto =
		(Protocol*)class_createInstance((Class)objc_getClass("Protocol"),
				sizeof(struct objc_protocol) - sizeof(id));
	proto->name = p->name;
	// Aliasing this means that when this returns these will all be updated.
	proto->protocol_list = p->protocol_list;
	proto->instance_methods = p->instance_methods;
	proto->class_methods = p->class_methods;
	assert(proto->isa);
	return proto;
}

PRIVATE struct objc_protocol *objc_upgrade_protocol_gsv1(struct objc_protocol_gsv1 *p)
{
	// If the protocol has already been upgraded, the don't try to upgrade it twice.
	if (p->isa == objc_getClass("Protocol"))
	{
		return (struct objc_protocol*)p;
	}
	if (p->properties)
	{
		p->properties = (struct objc_property_list_gsv1*)upgradePropertyList(p->properties);
	}
	if (p->optional_properties)
	{
		p->optional_properties = (struct objc_property_list_gsv1*)upgradePropertyList(p->optional_properties);
	}
	p->isa = objc_getClass("Protocol");
	assert(p->isa);
	return (struct objc_protocol*)p;
}

