/*****************************************************************************
 * en50221.h
 *****************************************************************************
 * Copyright (C) 2008 VideoLAN
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details.
 *****************************************************************************/

#include <stddef.h>

typedef void * access_t;

#define STRINGIFY( z )   UGLY_KLUDGE( z )
#define UGLY_KLUDGE( z ) #z

#define EN50221_MMI_NONE 0
#define EN50221_MMI_ENQ 1
#define EN50221_MMI_ANSW 2
#define EN50221_MMI_MENU 3
#define EN50221_MMI_MENU_ANSW 4
#define EN50221_MMI_LIST 5

typedef struct en50221_mmi_object_t
{
    int i_object_type;

    union
    {
        struct
        {
            int b_blind;
            char *psz_text;
        } enq;

        struct
        {
            int b_ok;
            char *psz_answ;
        } answ;

        struct
        {
            char *psz_title, *psz_subtitle, *psz_bottom;
            char **ppsz_choices;
            int i_choices;
        } menu; /* menu and list are the same */

        struct
        {
            int i_choice;
        } menu_answ;
    } u;
} en50221_mmi_object_t;

#define MAX_CI_SLOTS 16
#define MAX_SESSIONS 32

extern int i_ca_handle;
extern int i_ca_type;

/*****************************************************************************
 * Prototypes
 *****************************************************************************/
void en50221_Init( void );
void en50221_Reset( void );
void en50221_AddPMT( uint8_t *p_pmt );
void en50221_UpdatePMT( uint8_t *p_pmt );
void en50221_DeletePMT( uint8_t *p_pmt );
uint8_t en50221_StatusMMI( uint8_t *p_answer, ssize_t *pi_size );
uint8_t en50221_StatusMMISlot( uint8_t *p_buffer, ssize_t i_size,
                               uint8_t *p_answer, ssize_t *pi_size );
uint8_t en50221_OpenMMI( uint8_t *p_buffer, ssize_t i_size );
uint8_t en50221_CloseMMI( uint8_t *p_buffer, ssize_t i_size );
uint8_t en50221_GetMMIObject( uint8_t *p_buffer, ssize_t i_size,
                              uint8_t *p_answer, ssize_t *pi_size );
uint8_t en50221_SendMMIObject( uint8_t *p_buffer, ssize_t i_size );


/*
 * This is where it gets scary: do not show to < 18 yrs old
 */

/*****************************************************************************
 * en50221_SerializeMMIObject :
 *****************************************************************************/
static inline int en50221_SerializeMMIObject( uint8_t *p_answer,
                                              ssize_t *pi_size,
                                              en50221_mmi_object_t *p_object )
{
    ssize_t i_max_size = *pi_size;
    en50221_mmi_object_t *p_serialized = (en50221_mmi_object_t *)p_answer;
    char **pp_tmp;
    int i;

#define STORE_MEMBER(pp_pointer, i_size)                                \
    if ( i_size + *pi_size > i_max_size )                               \
        return -1;                                                      \
    memcpy( p_answer, *pp_pointer, i_size );                            \
    *pp_pointer = (void *)*pi_size;                                     \
    *pi_size += i_size;                                                 \
    p_answer += i_size;

    if ( sizeof(en50221_mmi_object_t) > i_max_size )
        return -1;
    memcpy( p_answer, p_object, sizeof(en50221_mmi_object_t) );
    *pi_size = sizeof(en50221_mmi_object_t);
    p_answer += sizeof(en50221_mmi_object_t);

    switch ( p_object->i_object_type )
    {
    case EN50221_MMI_ENQ:
        STORE_MEMBER( &p_serialized->u.enq.psz_text,
                      strlen(p_object->u.enq.psz_text) + 1 );
        break;

    case EN50221_MMI_ANSW:
        STORE_MEMBER( &p_serialized->u.answ.psz_answ,
                      strlen(p_object->u.answ.psz_answ) + 1 );
        break;

    case EN50221_MMI_MENU:
    case EN50221_MMI_LIST:
        STORE_MEMBER( &p_serialized->u.menu.psz_title,
                      strlen(p_object->u.menu.psz_title) + 1 );
        STORE_MEMBER( &p_serialized->u.menu.psz_subtitle,
                      strlen(p_object->u.menu.psz_subtitle) + 1 );
        STORE_MEMBER( &p_serialized->u.menu.psz_bottom,
                      strlen(p_object->u.menu.psz_bottom) + 1 );
        /* pointer alignment */
        i = ((*pi_size + 7) / 8) * 8 - *pi_size;
        *pi_size += i;
        p_answer += i;
        pp_tmp = (char **)p_answer;
        STORE_MEMBER( &p_serialized->u.menu.ppsz_choices,
                      p_object->u.menu.i_choices * sizeof(char *) );

        for ( i = 0; i < p_object->u.menu.i_choices; i++ )
        {
            STORE_MEMBER( &pp_tmp[i],
                          strlen(p_object->u.menu.ppsz_choices[i]) + 1 );
        }
        break;

    default:
        break;
    }

    return 0;
}

/*****************************************************************************
 * en50221_UnserializeMMIObject :
 *****************************************************************************/
static inline int en50221_UnserializeMMIObject( en50221_mmi_object_t *p_object,
                                                ssize_t i_size )
{
    int i, j;

#define CHECK_MEMBER(pp_member)                                         \
    if ( (ptrdiff_t)*pp_member >= i_size )                              \
        return -1;                                                      \
    for ( i = 0; ((char *)p_object + (ptrdiff_t)*pp_member)[i] != '\0'; \
          i++ )                                                         \
        if ( (ptrdiff_t)*pp_member + i >= i_size )                      \
            return -1;                                                  \
    *pp_member += (ptrdiff_t)p_object;

    switch ( p_object->i_object_type )
    {
    case EN50221_MMI_ENQ:
        CHECK_MEMBER(&p_object->u.enq.psz_text);
        break;

    case EN50221_MMI_ANSW:
        CHECK_MEMBER(&p_object->u.answ.psz_answ);
        break;

    case EN50221_MMI_MENU:
    case EN50221_MMI_LIST:
        CHECK_MEMBER(&p_object->u.menu.psz_title);
        CHECK_MEMBER(&p_object->u.menu.psz_subtitle);
        CHECK_MEMBER(&p_object->u.menu.psz_bottom);
        if ( (ptrdiff_t)p_object->u.menu.ppsz_choices
              + p_object->u.menu.i_choices * sizeof(char *) >= i_size )
            return -1;
        p_object->u.menu.ppsz_choices = (char **)((char *)p_object
                                 + (ptrdiff_t)p_object->u.menu.ppsz_choices);

        for ( j = 0; j < p_object->u.menu.i_choices; j++ )
        {
            CHECK_MEMBER(&p_object->u.menu.ppsz_choices[j]);
        }
        break;

    default:
        break;
    }

    return 0;
}
