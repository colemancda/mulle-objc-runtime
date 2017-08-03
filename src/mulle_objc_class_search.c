//
//  mulle_objc_class_search.c
//  mulle-objc-runtime
//
//  Created by Nat! on 17/08/03.
//  Copyright (c) 2014-2017 Nat! - Mulle kybernetiK.
//  Copyright (c) 2014-2017 Codeon GmbH.
//  All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are met:
//
//  Redistributions of source code must retain the above copyright notice, this
//  list of conditions and the following disclaimer.
//
//  Redistributions in binary form must reproduce the above copyright notice,
//  this list of conditions and the following disclaimer in the documentation
//  and/or other materials provided with the distribution.
//
//  Neither the name of Mulle kybernetiK nor the names of its contributors
//  may be used to endorse or promote products derived from this software
//  without specific prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
//  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
//  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//  POSSIBILITY OF SUCH DAMAGE.
//
#include "mulle_objc_class_search.h"

#include "mulle_objc_class.h"
#include "mulle_objc_class_struct.h"
#include "mulle_objc_class_universe.h"
#include "mulle_objc_classpair.h"
#include "mulle_objc_infraclass.h"
#include "mulle_objc_metaclass.h"
#include "mulle_objc_method.h"
#include "mulle_objc_methodlist.h"
#include "mulle_objc_universe.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <mulle_concurrent/mulle_concurrent.h>


#define OFFSET_2_MODE      100
#define OFFSET_3_MODE      200

enum internal_search_mode
{
   search_default             = MULLE_OBJC_SEARCH_DEFAULT,

   search_overridden_method   = MULLE_OBJC_SEARCH_OVERRIDDEN_METHOD,
   search_previous_method     = MULLE_OBJC_SEARCH_PREVIOUS_METHOD,
   search_specific_method     = MULLE_OBJC_SEARCH_SPECIFIC_METHOD,
   search_super_method        = MULLE_OBJC_SEARCH_SUPER_METHOD,
   search_supreme_method      = MULLE_OBJC_SEARCH_SUPREME_METHOD,

   search_overridden_method_2 = OFFSET_2_MODE + search_overridden_method,
   search_previous_method_2   = OFFSET_2_MODE + search_previous_method,
   search_specific_method_2   = OFFSET_2_MODE + search_specific_method,
   search_super_method_2      = OFFSET_2_MODE + search_super_method,
   search_supreme_method_2    = OFFSET_2_MODE + search_supreme_method,

   search_overridden_method_3 = OFFSET_3_MODE + search_overridden_method,
   search_specific_method_3   = OFFSET_3_MODE + search_specific_method
};

static struct _mulle_objc_method   *
__mulle_objc_class_search_method( struct _mulle_objc_class *cls,
                                  struct _mulle_objc_searcharguments *search,
                                  unsigned int inheritance,
                                  struct _mulle_objc_searchresult *result,
                                  enum internal_search_mode *mode);


#pragma mark - trace support


static void   trace_method_found( struct _mulle_objc_universe *universe,
                                 struct _mulle_objc_class *cls,
                                 struct _mulle_objc_methodlist *list,
                                 struct _mulle_objc_method *method,
                                 struct mulle_concurrent_pointerarrayreverseenumerator *rover)
{
   char                      buf[ s_mulle_objc_sprintf_functionpointer_buffer + 32];
   mulle_objc_categoryid_t   categoryid;
   char                      *s;
   
   fprintf( stderr, "mulle_objc_universe %p trace: found in %s ",
           universe,
           _mulle_objc_class_get_classtypename( cls));
   
   // it's a category ?
   if( list->owner)
   {
      categoryid = (mulle_objc_categoryid_t) (uintptr_t) list->owner;
      s = _mulle_objc_universe_search_debughashname( universe, categoryid);
      if( ! s)
      {
         sprintf( buf, "%08x", categoryid);
         s = buf;
      }
      
      fprintf( stderr, "category %s( %s) implementation ",
              cls->name,
              s);
   }
   else
      fprintf( stderr, "\"%s\" implementation ",
              cls->name);
   mulle_objc_sprintf_functionpointer( buf, (mulle_functionpointer_t) _mulle_objc_method_get_implementation( method));
   
   fprintf( stderr, "%s for methodid %08x ( \"%s\")\"\n",
           buf,
           method->descriptor.methodid,
           method->descriptor.name);
}


static void   trace_search( struct _mulle_objc_universe *universe,
                            struct _mulle_objc_class *cls,
                            struct _mulle_objc_searcharguments *search,
                            enum internal_search_mode mode)
{
   fprintf( stderr, "mulle_objc_universe %p trace: search %08x %s\"%s\" methodid %08x \"%s\"",
           universe,
           cls->classid,
           _mulle_objc_class_get_classtypename( cls),
           cls->name,
           search->args.methodid,
           _mulle_objc_universe_string_for_methodid( universe, search->args.methodid));
   
   switch( mode)
   {
   case search_previous_method   :
      fprintf( stderr, " (previous method=%p (IMP=%p))\n",
              search->previous_method,
              _mulle_objc_method_get_implementation( search->previous_method));
      return;
      
   case search_specific_method :
      fprintf( stderr, " (specific=%08x,%08x)\n",
              search->args.classid,
              search->args.categoryid);
      return;
      
   case search_super_method :
      fprintf( stderr, " (super=%08x)\n",
              search->args.classid);
      return;
      
   case search_supreme_method :
      fprintf( stderr, " (supreme=%08x)\n",
              search->args.classid);
      return;
      
   case search_overridden_method :
      fprintf( stderr, " (overridden=%08x,%08x)\n",
              search->args.classid,
              search->args.categoryid);
      return;
   }
   fprintf( stderr, "\n");
}


#pragma mark - method searching

#define MULLE_OBJC_METHOD_SEARCH_FAIL  ((struct _mulle_objc_method *) -1)

static struct _mulle_objc_method  *
_mulle_objc_class_protocol_search_method( struct _mulle_objc_class *cls,
                                          struct _mulle_objc_searcharguments *search,
                                          unsigned int inheritance,
                                          struct _mulle_objc_searchresult *result,
                                          enum internal_search_mode *mode)
{
   struct _mulle_objc_classpair                 *pair;
   struct _mulle_objc_infraclass                *infra;
   struct _mulle_objc_class                     *walk_cls;
   struct _mulle_objc_infraclass                *proto_cls;
   struct _mulle_objc_protocolclassenumerator   rover;
   struct _mulle_objc_method                    *found;
   struct _mulle_objc_method                    *method;
   int                                          is_meta;
   
   found   = MULLE_OBJC_METHOD_SEARCH_FAIL;
   pair    = _mulle_objc_class_get_classpair( cls);
   infra   = _mulle_objc_classpair_get_infraclass( pair);
   is_meta = _mulle_objc_class_is_metaclass( cls);
   
   rover = _mulle_objc_classpair_enumerate_protocolclasses( pair);
   while( proto_cls = _mulle_objc_protocolclassenumerator_next( &rover))
   {
      if( proto_cls == infra)
         continue;
      
      walk_cls = _mulle_objc_infraclass_as_class( proto_cls);
      if( is_meta)
         walk_cls = _mulle_objc_metaclass_as_class( _mulle_objc_infraclass_get_metaclass( proto_cls));
      
      method = __mulle_objc_class_search_method( walk_cls,
                                                 search,
                                                 inheritance | walk_cls->inheritance,
                                                 result,
                                                 mode);
      if( method == MULLE_OBJC_METHOD_SEARCH_FAIL)
         continue;
      
      if( ! method)
         return( NULL);
      
      if( found != MULLE_OBJC_METHOD_SEARCH_FAIL)
      {
         errno = EEXIST;
         return( NULL);
      }
      
      if( ! _mulle_objc_methoddescriptor_is_hidden_override_fatal( &method->descriptor))
         return( method);
      
      found = method;
   }
   _mulle_objc_protocolclassenumerator_done( &rover);
   
   return( found);
}



//
// if previous is set, the search will be done for the method that previous
// has overriden.
// Returns NULL : error (and sets errno)
//         MULLE_OBJC_METHOD_SEARCH_FAIL for not found
//         method : when found
//
static struct _mulle_objc_method   *
   __mulle_objc_class_search_method( struct _mulle_objc_class *cls,
                                     struct _mulle_objc_searcharguments *search,
                                     unsigned int inheritance,
                                     struct _mulle_objc_searchresult *result,
                                     enum internal_search_mode *mode)
{
   struct _mulle_objc_universe                             *universe;
   struct _mulle_objc_method                               *found;
   struct _mulle_objc_method                               *method;
   struct _mulle_objc_methodlist                           *list;
   struct mulle_concurrent_pointerarrayreverseenumerator   rover;
   unsigned int                                            n;
   unsigned int                                            tmp;
   
   
   // only enable first (@implementation of class) on demand
   //  ->[0]    : implementation
   //  ->[1]    : category
   //  ->[n -1] : last category
   
   found    = MULLE_OBJC_METHOD_SEARCH_FAIL;
   universe = _mulle_objc_class_get_universe( cls);
   if( universe->debug.trace.method_searches)
      trace_search( universe, cls, search, *mode);
   
   switch( *mode)
   {
   case search_super_method      :
   case search_overridden_method :
   case search_specific_method   :
      if( search->args.classid != cls->classid)
         goto next_class;
      *mode += OFFSET_2_MODE;
      break;
      
   case search_supreme_method    :
      if( search->args.classid == cls->classid)
         *mode += OFFSET_2_MODE;
      goto next_class;
   }
   
   n = mulle_concurrent_pointerarray_get_count( &cls->methodlists);
   assert( n);
   if( inheritance & MULLE_OBJC_CLASS_DONT_INHERIT_CATEGORIES)
      n = 1;
   
   rover = mulle_concurrent_pointerarray_reverseenumerate( &cls->methodlists, n);
   while( list = _mulle_concurrent_pointerarrayreverseenumerator_next( &rover))
   {
      switch( *mode)
      {
      case search_overridden_method_2 :
         if( (void *) (intptr_t) search->args.categoryid == list->owner)
            *mode += OFFSET_3_MODE - OFFSET_2_MODE;
         continue;

      case search_specific_method_2   :
         if( (void *) (intptr_t) search->args.categoryid != list->owner)
            continue;
         *mode += OFFSET_3_MODE - OFFSET_2_MODE;
         break;
      }

      // this returns NULL when not found!, there
      method = _mulle_objc_methodlist_search( list, search->args.methodid);
      if( ! method)
      {
         if( *mode == search_specific_method_3)
            return( MULLE_OBJC_METHOD_SEARCH_FAIL);
         continue;
      }
      
      switch( *mode)
      {
      case search_specific_method_3 :
         return( method);

      case search_previous_method :
         if( search->previous_method == method)
         {
            *mode = search_previous_method_2;
            continue;
         }
      }
      
      if( found != MULLE_OBJC_METHOD_SEARCH_FAIL)
      {
         errno = EEXIST;
         return( NULL);
      }
      
      if( result)
      {
         result->class  = cls;
         result->list   = list;
         result->method = method;
      }
      
      if( ! _mulle_objc_methoddescriptor_is_hidden_override_fatal( &method->descriptor))
      {
         // atomicity needed or not ? see header for more discussion
         method->descriptor.bits |= _mulle_objc_method_searched_and_found;
         
         if( universe->debug.trace.method_searches)
            trace_method_found( universe, cls, list, method, &rover);
         
         mulle_concurrent_pointerarrayreverseenumerator_done( &rover);
         return( method);
      }
      
      found = method;
   }
   mulle_concurrent_pointerarrayreverseenumerator_done( &rover);

   //
   // if we didn't find any starting point, it's not good for some modes
   //
   switch( *mode)
   {
   case search_overridden_method_2 :
   case search_specific_method_2   :
      errno = EINVAL;
      return( NULL);
   }
   
next_class:
   if( ! (inheritance & MULLE_OBJC_CLASS_DONT_INHERIT_PROTOCOLS))
   {
      tmp = 0;
      if( inheritance & MULLE_OBJC_CLASS_DONT_INHERIT_PROTOCOL_CATEGORIES)
         tmp |= MULLE_OBJC_CLASS_DONT_INHERIT_CATEGORIES;
      
      //
      // A protocol could well have a category of the same name and it would
      // match, which would be unexpected or would it ? Probably not.
      // Generally: MULLE_OBJC_CLASS_DONT_INHERIT_PROTOCOL_CATEGORIES is not
      // enabled, so it is a non-issue.
      //
      method = _mulle_objc_class_protocol_search_method( cls,
                                                         search,
                                                         tmp,
                                                         result,
                                                         mode);
      if( method != MULLE_OBJC_METHOD_SEARCH_FAIL)
      {
         if( ! method)
            return( NULL);
         
         if( found != MULLE_OBJC_METHOD_SEARCH_FAIL)
         {
            errno = EEXIST;
            return( NULL);
         }
         
         if( ! _mulle_objc_methoddescriptor_is_hidden_override_fatal( &method->descriptor))
            return( method);
         
         found = method;
      }
   }
   
   //
   // searching the superclass for owner seems wanted
   //
   if( ! (inheritance & MULLE_OBJC_CLASS_DONT_INHERIT_SUPERCLASS))
   {
      if( cls->superclass)
      {
         method = __mulle_objc_class_search_method( cls->superclass,
                                                    search,
                                                    cls->superclass->inheritance,
                                                    result,
                                                    mode);
         if( method != MULLE_OBJC_METHOD_SEARCH_FAIL)
         {
            if( ! method)
               return( NULL);
            
            if( found != MULLE_OBJC_METHOD_SEARCH_FAIL)
            {
               errno = EEXIST;
               return( NULL);
            }
            
            if( ! _mulle_objc_methoddescriptor_is_hidden_override_fatal( &method->descriptor))
               return( method);
            
            found = method;
         }
      }
   }
   
   return( found);
}


# pragma mark - caching

MULLE_C_NEVER_INLINE
static struct _mulle_objc_searchcacheentry   *
   _mulle_objc_class_fill_searchcache_with_methodsearch( struct _mulle_objc_class *cls,
                                                         struct _mulle_objc_method *method,
                                                         struct _mulle_objc_searchargumentscachable *args)
{
   struct _mulle_objc_searchcache        *cache;
   struct _mulle_objc_searchcacheentry   *entry;
   
   assert( cls);
   assert( method);
   
   // need to check that we are initialized
   if( ! _mulle_objc_class_get_state_bit( cls, MULLE_OBJC_CLASS_CACHE_READY))
   {
      _mulle_objc_universe_raise_inconsistency_exception( cls->universe, "call comes too early, the %s \"%s\" hasn't been initialized yet.", _mulle_objc_class_get_classtypename( cls), cls->name);
   }
   
   // when we trace method calls, we don't cache ever
   if( _mulle_objc_class_get_universe( cls)->debug.trace.method_calls)
      return( NULL);
   
   //
   //  try to get most up to date value
   //
   for(;;)
   {
      cache = _mulle_objc_searchcachepivot_atomic_get_cache( &cls->searchcachepivot);
      if( _mulle_objc_searchcache_should_grow( cache))
      {
         entry = _mulle_objc_class_add_searchcacheentry_by_swapping_caches( cls, cache, method, args);
         if( entry)
            return( entry);
         continue;
      }
      
      entry = _mulle_objc_searchcache_add_functionpointer_entry( cache, (mulle_functionpointer_t) _mulle_objc_method_get_implementation( method), args);
      if( entry)
         return( entry);
   }
}


// fills the cache and does forward
mulle_objc_methodimplementation_t
   _mulle_objc_class_lookup_methodsearch( struct _mulle_objc_class *cls,
                                          struct _mulle_objc_searchargumentscachable *args)
{
   mulle_objc_searchcache_uint_t         offset;
   mulle_objc_methodimplementation_t     imp;
   struct _mulle_objc_searchcache        *cache;
   struct _mulle_objc_searchcacheentry   *entries;
   struct _mulle_objc_searchcacheentry   *entry;
   struct _mulle_objc_method             *method;
   struct _mulle_objc_method             *forward;

   assert( args);
   assert( _mulle_objc_is_cacheablesearchmode( args->mode));
   _mulle_objc_searchargumentscacheable_assert( args);
   
   entries = _mulle_objc_searchcachepivot_atomic_get_entries( &cls->searchcachepivot);
   cache   = _mulle_objc_searchcacheentry_get_cache_from_entries( entries);
   offset  = _mulle_objc_searchcache_find_entryoffset( cache, args);
   entry   = (void *) &((char *) entries)[ offset];
   imp     = (mulle_objc_methodimplementation_t) _mulle_atomic_functionpointer_nonatomic_read( &entry->value.functionpointer);
   forward = _mulle_objc_class_unfailinggetorsearch_forwardmethod( cls, args->methodid);
   if( imp)
   {
      if( _mulle_objc_method_get_implementation( forward) == imp)
         imp = 0;
      return( imp);
   }
   
   //
   // since "previous_method" in args will not be accessed" this is OK to cast
   // and obviously cheaper than making a copy
   //
   method = mulle_objc_class_search_method( cls,
                                            (struct _mulle_objc_searcharguments *) args,
                                            cls->inheritance,
                                            NULL);
   if( ! method)
      method = forward;
   else
      imp = _mulle_objc_method_get_implementation( method);

   _mulle_objc_class_fill_searchcache_with_methodsearch( cls, method, args);

   return( imp);
}


# pragma mark API

//
// wrapper function, to not expose internal use of MULLE_OBJC_METHOD_SEARCH_FAIL
//
struct _mulle_objc_method   *
   mulle_objc_class_search_method( struct _mulle_objc_class *cls,
                                   struct _mulle_objc_searcharguments *search,
                                   unsigned int inheritance,
                                   struct _mulle_objc_searchresult *result)
{
   struct _mulle_objc_method    *method;
   enum internal_search_mode    mode;
   
   if( ! cls || ! search)
   {
      errno = EINVAL;
      return( NULL);
   }

   _mulle_objc_searcharguments_assert( search);

   assert( mulle_objc_class_is_current_thread_registered( cls));

   mode = (enum internal_search_mode) search->args.mode;
   switch( mode)
   {
   case search_default           :
   case search_previous_method   :
   case search_specific_method   :
   case search_super_method      :
   case search_supreme_method    :
   case search_overridden_method :
      break;
      
   default :
      errno = EINVAL;
      return( NULL);
   }

   method = __mulle_objc_class_search_method( cls,
                                              search,
                                              inheritance,
                                              result,
                                              &mode);

   //
   // these are relative searches, if we are still
   // in the original mode, then the "search anchor"
   // has not been found, which is an error, as the class or
   // category is actually missing
   //
   switch( mode)
   {
   case search_previous_method   :
   case search_super_method      :
   case search_supreme_method    :
   case search_overridden_method :
      errno = EINVAL;
      return( NULL);
   }
   
   // this can happen if hidden override detection is on
   
   if( method == MULLE_OBJC_METHOD_SEARCH_FAIL)
   {
      if( errno == EEXIST)
         _mulle_objc_universe_raise_inconsistency_exception( cls->universe,
                              "class \"%s\" hidden method override of %08x",
                              _mulle_objc_class_get_name( cls),
                              search->args.methodid);
      
      errno = ENOENT;
      return( NULL);
   }
   
   return( method);
}


struct _mulle_objc_method  *
   mulle_objc_class_defaultsearch_method( struct _mulle_objc_class *cls,
                                          mulle_objc_methodid_t methodid)
{
   struct _mulle_objc_searcharguments   search;
   unsigned int                         inheritance;
   
   if( ! cls)
   {
      errno = EINVAL;
      return( NULL);
   }

   inheritance = _mulle_objc_class_get_inheritance( cls);
   _mulle_objc_searcharguments_defaultinit( &search, methodid);
   return( mulle_objc_class_search_method( cls,
                                           &search,
                                           inheritance,
                                           NULL));
}


struct _mulle_objc_method  *
   mulle_objc_class_search_non_inherited_method( struct _mulle_objc_class *cls,
                                                 mulle_objc_methodid_t methodid)
{
   unsigned int                         inheritance;
   struct _mulle_objc_searcharguments   search;
   
   if( ! cls)
   {
      errno = EINVAL;
      return( NULL);
   }

   inheritance = _mulle_objc_class_get_inheritance( cls) |
                    MULLE_OBJC_CLASS_DONT_INHERIT_SUPERCLASS;

   _mulle_objc_searcharguments_defaultinit( &search, methodid);
   return( mulle_objc_class_search_method( cls,
                                           &search,
                                           inheritance,
                                           NULL));
}

# pragma mark - forwarding

static inline struct _mulle_objc_method   *
   _mulle_objc_class_search_forwardmethod( struct _mulle_objc_class *cls)
{
   struct _mulle_objc_method   *method;
   
   method = mulle_objc_class_defaultsearch_method( cls, MULLE_OBJC_FORWARD_METHODID);
   if( ! method)
      method = cls->universe->classdefaults.forwardmethod;
   
   return( method);
}


struct _mulle_objc_method    *
   _mulle_objc_class_getorsearch_forwardmethod( struct _mulle_objc_class *cls)
{
   struct _mulle_objc_method   *method;
   
   assert( mulle_objc_class_is_current_thread_registered( cls));
   
   method = _mulle_objc_class_get_forwardmethod( cls);
   if( ! method)
   {
      method = _mulle_objc_class_search_forwardmethod( cls);
      if( method)
         _mulle_objc_class_set_forwardmethod( cls, method);
   }
   return( method);
}

      
MULLE_C_NO_RETURN
static void
   _mulle_objc_class_raise_method_not_found( struct _mulle_objc_class *cls,
                                             mulle_objc_methodid_t missing_method)
{
   char   *prefix;
   char   *name;
   
   prefix = _mulle_objc_class_is_metaclass( cls) ? "meta-" : "";
   name   = _mulle_objc_class_get_name( cls);
   
   if( errno != ENOENT)
      _mulle_objc_universe_raise_inconsistency_exception( cls->universe, "mulle_objc_universe: forward:: method has wrong id in %sclass \"%s\"",
                                                         prefix,
                                                         name);
   if( missing_method)
      _mulle_objc_class_raise_method_not_found_exception( cls, missing_method);
   
   _mulle_objc_universe_raise_inconsistency_exception( cls->universe, "mulle_objc_universe: missing forward:: method in %sclass \"%s\"",
                                                      prefix,
                                                      name);
}


MULLE_C_NON_NULL_RETURN
struct _mulle_objc_method    *
   _mulle_objc_class_unfailinggetorsearch_forwardmethod( struct _mulle_objc_class *cls,
                                                          mulle_objc_methodid_t   missing_method)
{
   struct _mulle_objc_method   *method;
   
   method = _mulle_objc_class_getorsearch_forwardmethod( cls);
   if( method)
      return( method);
   
   _mulle_objc_class_raise_method_not_found( cls, missing_method);
}


// used by the debugger
mulle_objc_methodimplementation_t
mulle_objc_class_get_forwardmethodimplementation( struct _mulle_objc_class *cls)
{
   struct _mulle_objc_method   *method;
   
   if( ! cls)
      return( 0);
   method = _mulle_objc_class_search_forwardmethod( cls);
   if( method)
      return( _mulle_objc_method_get_implementation( method));
   return( 0);
}

