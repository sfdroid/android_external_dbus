/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-binding-tool-glib.c: Output C glue
 *
 * Copyright (C) 2003, 2004, 2005 Red Hat, Inc.
 *
 * Licensed under the Academic Free License version 2.1
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <config.h>
#include "dbus-gidl.h"
#include "dbus-gparser.h"
#include "dbus-gutils.h"
#include "dbus-gvalue.h"
#include "dbus-gvalue-utils.h"
#include "dbus-glib-tool.h"
#include "dbus-binding-tool-glib.h"
#include <glib/gi18n.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MARSHAL_PREFIX "dbus_glib_marshal_"

typedef struct
{
  gboolean ignore_unsupported;
  const char* prefix;
  GIOChannel *channel;
  
  GError **error;
  
  GHashTable *generated;
  GString *blob;
  guint count;
} DBusBindingToolCData;

static gboolean gather_marshallers (BaseInfo *base, DBusBindingToolCData *data, GError **error);
static gboolean generate_glue (BaseInfo *base, DBusBindingToolCData *data, GError **error);
static gboolean generate_client_glue (BaseInfo *base, DBusBindingToolCData *data, GError **error);

static const char *
dbus_g_type_get_marshal_name (GType gtype)
{
  switch (G_TYPE_FUNDAMENTAL (gtype))
    {
    case G_TYPE_BOOLEAN:
      return "BOOLEAN";
    case G_TYPE_UCHAR:
      return "UCHAR";
    case G_TYPE_INT:
      return "INT";
    case G_TYPE_UINT:
      return "UINT";
    case G_TYPE_INT64:
      return "INT64";
    case G_TYPE_UINT64:
      return "UINT64";
    case G_TYPE_DOUBLE:
      return "DOUBLE";
    case G_TYPE_STRING:
      return "STRING";
    case G_TYPE_POINTER:
      return "POINTER";
    case G_TYPE_BOXED:
      return "BOXED";
    case G_TYPE_OBJECT:
      return "OBJECT";
    default:
      return NULL;
    }
}

/* This entire function is kind of...ugh. */
static const char *
dbus_g_type_get_c_name (GType gtype)
{
  if (dbus_g_type_is_collection (gtype))
    return "GArray";
  if (dbus_g_type_is_map (gtype))
    return "GHashTable";
  
  if (g_type_is_a (gtype, G_TYPE_STRING))
    return "char *";

  /* This one is even more hacky...we get an extra *
   * because G_TYPE_STRV is a G_TYPE_BOXED
   */
  if (g_type_is_a (gtype, G_TYPE_STRV))
    return "char *";
  
  return g_type_name (gtype);
}

static char *
compute_marshaller (MethodInfo *method, GError **error)
{
  GSList *elt;
  GString *ret;
  gboolean first;

  /* All methods required to return boolean for now;
   * will be conditional on method info later */
  ret = g_string_new ("BOOLEAN:");

  first = TRUE;
  /* Append input arguments */
  for (elt = method_info_get_args (method); elt; elt = elt->next)
    {
      ArgInfo *arg = elt->data;

      if (arg_info_get_direction (arg) == ARG_IN)
	{
	  const char *marshal_name;
	  GType gtype;

	  gtype = dbus_gtype_from_signature (arg_info_get_type (arg), FALSE);
	  if (gtype == G_TYPE_INVALID)
	    {
	      g_set_error (error,
			   DBUS_BINDING_TOOL_ERROR,
			   DBUS_BINDING_TOOL_ERROR_UNSUPPORTED_CONVERSION,
			   _("Unsupported conversion from D-BUS type %s to glib-genmarshal type"),
			   arg_info_get_type (arg));
	      g_string_free (ret, TRUE);
	      return NULL;
	    }

	  marshal_name = dbus_g_type_get_marshal_name (gtype);
	  g_assert (marshal_name);

	  if (!first)
	    g_string_append (ret, ",");
	  else
	    first = FALSE;
	  g_string_append (ret, marshal_name);
	}
    }

  if (method_info_get_annotation (method, DBUS_GLIB_ANNOTATION_ASYNC) != NULL)
    {
      if (!first)
	g_string_append (ret, ",");
      g_string_append (ret, "POINTER");
      first = FALSE;
    }
  else
    {
      /* Append pointer for each out arg storage */
      for (elt = method_info_get_args (method); elt; elt = elt->next)
	{
	  ArgInfo *arg = elt->data;

	  if (arg_info_get_direction (arg) == ARG_OUT)
	    {
	      if (!first)
		g_string_append (ret, ",");
	      else
		first = FALSE;
	      g_string_append (ret, "POINTER");
	    }
	}
      /* Final GError parameter */
      if (!first)
	g_string_append (ret, ",");
      g_string_append (ret, "POINTER");

    }

  return g_string_free (ret, FALSE);

}

static char *
compute_marshaller_name (MethodInfo *method, const char *prefix, GError **error)
{
  GSList *elt;
  GString *ret;

  /* All methods required to return boolean for now;
   * will be conditional on method info later */
  ret = g_string_new (MARSHAL_PREFIX);
  g_string_append (ret, prefix);
  g_string_append (ret, "_BOOLEAN_");

  /* Append input arguments */
  for (elt = method_info_get_args (method); elt; elt = elt->next)
    {
      ArgInfo *arg = elt->data;

      if (arg_info_get_direction (arg) == ARG_IN)
	{
	  const char *marshal_name;
	  const char *type;
	  GType gtype;

	  type = arg_info_get_type (arg);
	  gtype = dbus_gtype_from_signature (type, FALSE);
	  if (gtype == G_TYPE_INVALID)
	    {
	      g_set_error (error,
			   DBUS_BINDING_TOOL_ERROR,
			   DBUS_BINDING_TOOL_ERROR_UNSUPPORTED_CONVERSION,
			   _("Unsupported conversion from D-BUS type %s to glib type"),
			   type);
	      g_string_free (ret, TRUE);
	      return NULL;
	    }
	  marshal_name = dbus_g_type_get_marshal_name (gtype);
	  g_assert (marshal_name != NULL);

	  g_string_append (ret, "_");
	  g_string_append (ret, marshal_name);
	}
    }

  if (method_info_get_annotation (method, DBUS_GLIB_ANNOTATION_ASYNC) != NULL)
    {
      g_string_append (ret, "_POINTER");
    }
  else
    {
      /* Append pointer for each out arg storage */
      for (elt = method_info_get_args (method); elt; elt = elt->next)
	{
	  ArgInfo *arg = elt->data;

	  if (arg_info_get_direction (arg) == ARG_OUT)
	    {
	      g_string_append (ret, "_POINTER");
	    }
	}
      /* Final GError parameter */
      g_string_append (ret, "_POINTER");
    }

  return g_string_free (ret, FALSE);
}

static gboolean
gather_marshallers_list (GSList *list, DBusBindingToolCData *data, GError **error)
{
  GSList *tmp;

  tmp = list;
  while (tmp != NULL)
    {
      if (!gather_marshallers (tmp->data, data, error))
	return FALSE;
      tmp = tmp->next;
    }
  return TRUE;
}

static gboolean
gather_marshallers (BaseInfo *base, DBusBindingToolCData *data, GError **error)
{
  if (base_info_get_type (base) == INFO_TYPE_NODE)
    {
      if (!gather_marshallers_list (node_info_get_nodes ((NodeInfo *) base),
				    data, error))
	return FALSE;
      if (!gather_marshallers_list (node_info_get_interfaces ((NodeInfo *) base),
				    data, error))
	return FALSE;
    }
  else
    {
      InterfaceInfo *interface;
      GSList *methods;
      GSList *tmp;
      const char *interface_c_name;

      interface = (InterfaceInfo *) base;
      interface_c_name = interface_info_get_annotation (interface, DBUS_GLIB_ANNOTATION_C_SYMBOL);
      if (interface_c_name == NULL)
        {
	  if (!data->prefix)
	    return TRUE;
        }

      methods = interface_info_get_methods (interface);

      /* Generate the necessary marshallers for the methods. */

      for (tmp = methods; tmp != NULL; tmp = g_slist_next (tmp))
        {
          MethodInfo *method;
          char *marshaller_name;

          method = (MethodInfo *) tmp->data;

          marshaller_name = compute_marshaller (method, error);
	  if (!marshaller_name)
	    return FALSE;

	  if (g_hash_table_lookup (data->generated, marshaller_name))
	    {
	      g_free (marshaller_name);
	      continue;
	    }

	  g_hash_table_insert (data->generated, marshaller_name, NULL);
        }

    }
  return TRUE;
}

static gboolean
generate_glue_list (GSList *list, DBusBindingToolCData *data, GError **error)
{
  GSList *tmp;

  tmp = list;
  while (tmp != NULL)
    {
      if (!generate_glue (tmp->data, data, error))
	return FALSE;
      tmp = tmp->next;
    }
  return TRUE;
}

#define WRITE_OR_LOSE(x) do { gsize bytes_written; if (!g_io_channel_write_chars (channel, x, -1, &bytes_written, error)) goto io_lose; } while (0)

static gboolean
write_printf_to_iochannel (const char *fmt, GIOChannel *channel, GError **error, ...)
{
  char *str;
  va_list args;
  GIOStatus status;
  gsize written;
  gboolean ret;

  va_start (args, error);

  str = g_strdup_vprintf (fmt, args);
  if ((status = g_io_channel_write_chars (channel, str, -1, &written, error)) == G_IO_STATUS_NORMAL)
    ret = TRUE;
  else
    ret = FALSE;

  g_free (str);

  va_end (args);

  return ret;
}

static gboolean
generate_glue (BaseInfo *base, DBusBindingToolCData *data, GError **error)
{
  if (base_info_get_type (base) == INFO_TYPE_NODE)
    {
      GString *object_introspection_data_blob;
      GIOChannel *channel;
      guint i;

      channel = data->channel;
      
      object_introspection_data_blob = g_string_new_len ("", 0);
      
      data->blob = object_introspection_data_blob;
      data->count = 0;

      if (!write_printf_to_iochannel ("static const DBusGMethodInfo dbus_glib_%s_methods[] = {\n", channel, error, data->prefix))
	goto io_lose;

      if (!generate_glue_list (node_info_get_nodes ((NodeInfo *) base),
			       data, error))
	return FALSE;
      if (!generate_glue_list (node_info_get_interfaces ((NodeInfo *) base),
			       data, error))
	return FALSE;

      WRITE_OR_LOSE ("};\n\n");

      /* Information about the object. */

      if (!write_printf_to_iochannel ("const DBusGObjectInfo dbus_glib_%s_object_info = {\n",
				      channel, error, data->prefix))
	goto io_lose;
      WRITE_OR_LOSE ("  0,\n");
      if (!write_printf_to_iochannel ("  dbus_glib_%s_methods,\n", channel, error, data->prefix))
	goto io_lose;
      if (!write_printf_to_iochannel ("  %d,\n", channel, error, data->count))
	goto io_lose;
      WRITE_OR_LOSE("  \"");
      for (i = 0; i < object_introspection_data_blob->len; i++)
	{
	  if (object_introspection_data_blob->str[i] != '\0')
	    {
	      if (!g_io_channel_write_chars (channel, object_introspection_data_blob->str + i, 1, NULL, error))
		return FALSE;
	    }
	  else
	    {
	      if (!g_io_channel_write_chars (channel, "\\0", -1, NULL, error))
		return FALSE;
	    }
	}
      WRITE_OR_LOSE ("\"\n};\n\n");

      g_string_free (object_introspection_data_blob, TRUE);
    }
  else
    {
      GIOChannel *channel;
      InterfaceInfo *interface;
      GSList *methods;
      GSList *tmp;
      const char *interface_c_name;
      GString *object_introspection_data_blob;

      channel = data->channel;
      object_introspection_data_blob = data->blob;

      interface = (InterfaceInfo *) base;
      interface_c_name = interface_info_get_annotation (interface, DBUS_GLIB_ANNOTATION_C_SYMBOL);
      if (interface_c_name == NULL)
        {
	  if (data->prefix == NULL)
	    return TRUE;
	  interface_c_name = data->prefix;
        }

      methods = interface_info_get_methods (interface);

      /* Table of marshalled methods. */

      for (tmp = methods; tmp != NULL; tmp = g_slist_next (tmp))
        {
          MethodInfo *method;
          char *marshaller_name;
	  char *method_c_name;
          gboolean async = FALSE;
	  GSList *args;

          method = (MethodInfo *) tmp->data;
	  method_c_name = g_strdup (method_info_get_annotation (method, DBUS_GLIB_ANNOTATION_C_SYMBOL));
          if (method_c_name == NULL)
	    {
	      char *method_name_uscored;
	      method_name_uscored = _dbus_gutils_wincaps_to_uscore (method_info_get_name (method));
              method_c_name = g_strdup_printf ("%s_%s",
					       interface_c_name,
					       method_name_uscored);
	      g_free (method_name_uscored);
            }

          if (!write_printf_to_iochannel ("  { (GCallback) %s, ", channel, error,
					  method_c_name))
	    goto io_lose;

          marshaller_name = compute_marshaller_name (method, data->prefix, error);
	  if (!marshaller_name)
	    goto io_lose;

          if (!write_printf_to_iochannel ("%s, %d },\n", channel, error,
					  marshaller_name,
					  object_introspection_data_blob->len))
	    {
	      g_free (marshaller_name);
	      goto io_lose;
	    }

          if (method_info_get_annotation (method, DBUS_GLIB_ANNOTATION_ASYNC) != NULL)
            async = TRUE;

	  /* Object method data blob format:
	   * <iface>\0<name>\0(<argname>\0<argdirection>\0<argtype>\0)*\0
	   */

	  g_string_append (object_introspection_data_blob, interface_info_get_name (interface));
	  g_string_append_c (object_introspection_data_blob, '\0');

	  g_string_append (object_introspection_data_blob, method_info_get_name (method));
	  g_string_append_c (object_introspection_data_blob, '\0');

	  g_string_append_c (object_introspection_data_blob, async ? 'A' : 'S');
	  g_string_append_c (object_introspection_data_blob, '\0');

	  for (args = method_info_get_args (method); args; args = args->next)
	    {
	      ArgInfo *arg;
	      char direction;

	      arg = args->data;

	      g_string_append (object_introspection_data_blob, arg_info_get_name (arg));
	      g_string_append_c (object_introspection_data_blob, '\0');

	      switch (arg_info_get_direction (arg))
		{
		case ARG_IN:
		  direction = 'I';
		  break;
		case ARG_OUT:
		  direction = 'O';
		  break;
		case ARG_INVALID:
                default:
                  g_assert_not_reached ();
                  direction = 0; /* silence gcc */
		  break;
		}
	      g_string_append_c (object_introspection_data_blob, direction);
	      g_string_append_c (object_introspection_data_blob, '\0');

	      g_string_append (object_introspection_data_blob, arg_info_get_type (arg));
	      g_string_append_c (object_introspection_data_blob, '\0');
	    }

	  g_string_append_c (object_introspection_data_blob, '\0');

          data->count++;
        }
    }
  return TRUE;
 io_lose:
  return FALSE;
}

static void
write_marshaller (gpointer key, gpointer value, gpointer user_data)
{
  DBusBindingToolCData *data;
  const char *marshaller;
  gsize bytes_written;

  data = user_data;
  marshaller = key;

  if (data->error && *data->error)
    return;

  if (g_io_channel_write_chars (data->channel, marshaller, -1, &bytes_written, data->error) == G_IO_STATUS_NORMAL)
    g_io_channel_write_chars (data->channel, "\n", -1, &bytes_written, data->error);
}

gboolean
dbus_binding_tool_output_glib_server (BaseInfo *info, GIOChannel *channel, const char *prefix, GError **error)
{
  gboolean ret;
  GPtrArray *argv;
  gint child_stdout;
  GIOChannel *genmarshal_stdout;
  GPid child_pid;
  DBusBindingToolCData data;
  char *tempfile_name;
  gint tempfile_fd;
  GIOStatus iostatus;
  char buf[4096];
  gsize bytes_read, bytes_written;

  memset (&data, 0, sizeof (data));

  dbus_g_value_types_init ();

  data.prefix = prefix;
  data.generated = g_hash_table_new_full (g_str_hash, g_str_equal, (GDestroyNotify) g_free, NULL);
  data.error = error;
  genmarshal_stdout = NULL;
  tempfile_name = NULL;

  if (!gather_marshallers (info, &data, error))
    goto io_lose;

  tempfile_fd = g_file_open_tmp ("dbus-binding-tool-c-marshallers.XXXXXX",
				 &tempfile_name, error);
  if (tempfile_fd < 0)
    goto io_lose;

  data.channel = g_io_channel_unix_new (tempfile_fd);
  if (!g_io_channel_set_encoding (data.channel, NULL, error))
    goto io_lose;
  g_hash_table_foreach (data.generated, write_marshaller, &data); 
  if (error && *error != NULL)
    {
      ret = FALSE;
      g_io_channel_close (data.channel);
      g_io_channel_unref (data.channel);
      goto io_lose;
    }

  g_io_channel_close (data.channel);
  g_io_channel_unref (data.channel);
  
  /* Now spawn glib-genmarshal to insert all our required marshallers */
  argv = g_ptr_array_new ();
  g_ptr_array_add (argv, "glib-genmarshal");
  g_ptr_array_add (argv, "--header");
  g_ptr_array_add (argv, "--body");
  g_ptr_array_add (argv, g_strdup_printf ("--prefix=%s%s", MARSHAL_PREFIX, prefix));
  g_ptr_array_add (argv, tempfile_name);
  g_ptr_array_add (argv, NULL);
  if (!g_spawn_async_with_pipes (NULL, (char**)argv->pdata, NULL,
				 G_SPAWN_SEARCH_PATH,
				 NULL, NULL,
				 &child_pid,
				 NULL,
				 &child_stdout, NULL, error))
    {
      g_ptr_array_free (argv, TRUE);
      goto io_lose;
    }
  g_ptr_array_free (argv, TRUE);

  genmarshal_stdout = g_io_channel_unix_new (child_stdout);
  if (!g_io_channel_set_encoding (genmarshal_stdout, NULL, error))
    goto io_lose;

  WRITE_OR_LOSE ("/* Generated by dbus-binding-tool; do not edit! */\n\n");

  while ((iostatus = g_io_channel_read_chars (genmarshal_stdout, buf, sizeof (buf),
					      &bytes_read, error)) == G_IO_STATUS_NORMAL)
    if (g_io_channel_write_chars (channel, buf, bytes_read, &bytes_written, error) != G_IO_STATUS_NORMAL)
      goto io_lose;
  if (iostatus != G_IO_STATUS_EOF)
    goto io_lose;

  g_io_channel_close (genmarshal_stdout);

  WRITE_OR_LOSE ("#include <dbus/dbus-glib.h>\n");

  data.channel = channel;
  g_io_channel_ref (data.channel);
  if (!generate_glue (info, &data, error))
    goto io_lose;
  
  ret = TRUE;
 cleanup:
  if (tempfile_name)
    unlink (tempfile_name);
  g_free (tempfile_name);
  if (genmarshal_stdout)
    g_io_channel_unref (genmarshal_stdout);
  if (data.channel)
    g_io_channel_unref (data.channel);
  g_hash_table_destroy (data.generated);

  return ret;
 io_lose:
  ret = FALSE;
  goto cleanup;
}

static char *
iface_to_c_prefix (const char *iface)
{
  char **components;
  char **component;
  GString *ret;
  gboolean first;
  
  components = g_strsplit (iface, ".", 0);

  first = TRUE;
  ret = g_string_new ("");
  for (component = components; *component; component++)
    {
      if (!first)
	g_string_append_c (ret, '_');
      else
	first = FALSE;
      g_string_append (ret, *component);
    }
  g_strfreev (components);
  return g_string_free (ret, FALSE);
}

static char *
compute_client_method_name (const char *iface_prefix, MethodInfo *method)
{
  GString *ret;
  char *method_name_uscored;

  ret = g_string_new (iface_prefix);
  
  method_name_uscored = _dbus_gutils_wincaps_to_uscore (method_info_get_name (method));
  g_string_append_c (ret, '_');
  g_string_append (ret, method_name_uscored);
  g_free (method_name_uscored);
  return g_string_free (ret, FALSE);
}

static gboolean
write_formal_parameters (InterfaceInfo *iface, MethodInfo *method, GIOChannel *channel, GError **error)
{
  GSList *args;

  for (args = method_info_get_args (method); args; args = args->next)
    {
      ArgInfo *arg;
      const char *type_str;
      const char *type_suffix;
      GType gtype;
      int direction;

      arg = args->data;

      WRITE_OR_LOSE (", ");

      direction = arg_info_get_direction (arg);

      gtype = dbus_gtype_from_signature (arg_info_get_type (arg), TRUE);
      if (gtype == G_TYPE_INVALID)
	{
	  g_set_error (error,
		       DBUS_BINDING_TOOL_ERROR,
		       DBUS_BINDING_TOOL_ERROR_UNSUPPORTED_CONVERSION,
		       _("Unsupported conversion from D-BUS type signature \"%s\" to glib C type in method \"%s\" of interface \"%s\""),
		       arg_info_get_type (arg),
		       method_info_get_name (method),
		       interface_info_get_name (iface));
	  return FALSE;
	}
      type_str = dbus_g_type_get_c_name (gtype);
      g_assert (type_str);
      /* Variants are special...*/
      if (gtype == G_TYPE_VALUE)
	{
	  if (direction == ARG_IN)
	    type_suffix = "*";
	  else
	    type_suffix = "";
	}
      else if ((g_type_is_a (gtype, G_TYPE_BOXED)
	      || g_type_is_a (gtype, G_TYPE_OBJECT)
	   || g_type_is_a (gtype, G_TYPE_POINTER)))
	type_suffix = "*";
      else
	type_suffix = "";


      switch (direction)
	{
	case ARG_IN:
	  if (!write_printf_to_iochannel ("const %s%s IN_%s", channel, error,
					  type_str,
					  type_suffix,
					  arg_info_get_name (arg)))
	    goto io_lose;
	  break;
	case ARG_OUT:
	  if (!write_printf_to_iochannel ("%s%s* OUT_%s", channel, error,
					  type_str,
					  type_suffix,
					  arg_info_get_name (arg)))
	    goto io_lose;
	  break;
	case ARG_INVALID:
	  break;
	}
    }

  return TRUE;
 io_lose:
  return FALSE;
}

#define MAP_FUNDAMENTAL(NAME) \
   case G_TYPE_ ## NAME: \
     return g_strdup ("G_TYPE_" #NAME);
#define MAP_KNOWN(NAME) \
    if (gtype == NAME) \
      return g_strdup (#NAME)
static char *
dbus_g_type_get_lookup_function (GType gtype)
{
  char *type_lookup;
  switch (gtype)
    {
      MAP_FUNDAMENTAL(CHAR);
      MAP_FUNDAMENTAL(UCHAR);
      MAP_FUNDAMENTAL(BOOLEAN);
      MAP_FUNDAMENTAL(LONG);
      MAP_FUNDAMENTAL(ULONG);
      MAP_FUNDAMENTAL(INT);
      MAP_FUNDAMENTAL(UINT);
      MAP_FUNDAMENTAL(INT64);
      MAP_FUNDAMENTAL(UINT64);
      MAP_FUNDAMENTAL(FLOAT);
      MAP_FUNDAMENTAL(DOUBLE);
      MAP_FUNDAMENTAL(STRING);
    }
  if (dbus_g_type_is_collection (gtype))
    {
      GType elt_gtype;
      char *sublookup;
      
      elt_gtype = dbus_g_type_get_collection_specialization (gtype);
      sublookup = dbus_g_type_get_lookup_function (elt_gtype);
      g_assert (sublookup);
      type_lookup = g_strdup_printf ("dbus_g_type_get_collection (\"GArray\", %s)",
				     sublookup);
      g_free (sublookup);
      return type_lookup;
    }
  else if (dbus_g_type_is_map (gtype))
    {
      GType key_gtype;
      char *key_lookup;
      GType value_gtype;
      char *value_lookup;
      
      key_gtype = dbus_g_type_get_map_key_specialization (gtype);
      value_gtype = dbus_g_type_get_map_value_specialization (gtype);
      key_lookup = dbus_g_type_get_lookup_function (key_gtype);
      g_assert (key_lookup);
      value_lookup = dbus_g_type_get_lookup_function (value_gtype);
      g_assert (value_lookup);
      type_lookup = g_strdup_printf ("dbus_g_type_get_map (\"GHashTable\", %s, %s)",
				     key_lookup, value_lookup);
      g_free (key_lookup);
      g_free (value_lookup);
      return type_lookup;
    }
  MAP_KNOWN(G_TYPE_VALUE);
  MAP_KNOWN(G_TYPE_STRV);
  MAP_KNOWN(DBUS_TYPE_G_PROXY);
  MAP_KNOWN(DBUS_TYPE_G_PROXY_ARRAY);
  return NULL;
}
#undef MAP_FUNDAMENTAL
#undef MAP_KNOWN

static gboolean
write_args_for_direction (InterfaceInfo *iface, MethodInfo *method, GIOChannel *channel, int direction, GError **error)
{
  GSList *args;

  for (args = method_info_get_args (method); args; args = args->next)
    {
      ArgInfo *arg;
      GType gtype;
      char *type_lookup;

      arg = args->data;

      if (direction != arg_info_get_direction (arg))
	continue;

      gtype = dbus_gtype_from_signature (arg_info_get_type (arg), TRUE);
      g_assert (gtype != G_TYPE_INVALID);
      type_lookup = dbus_g_type_get_lookup_function (gtype);
      g_assert (type_lookup != NULL);

      switch (direction)
	{

	case ARG_IN:
	  if (!write_printf_to_iochannel ("%s, IN_%s, ", channel, error,
					  type_lookup,
					  arg_info_get_name (arg)))
	    goto io_lose;
	  break;
	case ARG_OUT:
	  if (!write_printf_to_iochannel ("%s, OUT_%s, ", channel, error,
					  type_lookup,
					  arg_info_get_name (arg)))
	    goto io_lose;
	  break;
	case ARG_INVALID:
	  break;
	}
      g_free (type_lookup);
    }

  return TRUE;
 io_lose:
  return FALSE;
}

static gboolean
check_supported_parameters (MethodInfo *method)
{
  GSList *args;

  for (args = method_info_get_args (method); args; args = args->next)
    {
      ArgInfo *arg;
      GType gtype;

      arg = args->data;
      gtype = dbus_gtype_from_signature (arg_info_get_type (arg), TRUE);
      if (gtype == G_TYPE_INVALID)
	return FALSE;
    }
  return TRUE;
}

static gboolean
write_untyped_out_args (InterfaceInfo *iface, MethodInfo *method, GIOChannel *channel, GError **error)
{
  GSList *args;

  for (args = method_info_get_args (method); args; args = args->next)
    {
      ArgInfo *arg;

      arg = args->data;
      if (arg_info_get_direction (arg) != ARG_OUT)
        continue;
            
      if (!write_printf_to_iochannel ("OUT_%s, ", channel, error,
                                      arg_info_get_name (arg)))
        goto io_lose;
     }

   return TRUE;
 io_lose:
  return FALSE;
}

static gboolean
write_formal_declarations_for_direction (InterfaceInfo *iface, MethodInfo *method, GIOChannel *channel, const int direction, GError **error)
 {
   GSList *args;
 
   for (args = method_info_get_args (method); args; args = args->next)
     {
       ArgInfo *arg;
      GType gtype;
      const char *type_str, *type_suffix;
      int dir;

       arg = args->data;

      dir = arg_info_get_direction (arg);

      gtype = dbus_gtype_from_signature (arg_info_get_type (arg), TRUE);
      type_str = dbus_g_type_get_c_name (gtype);

      if (!type_str)
       {
         g_set_error (error,
                      DBUS_BINDING_TOOL_ERROR,
                      DBUS_BINDING_TOOL_ERROR_UNSUPPORTED_CONVERSION,
                      _("Unsupported conversion from D-BUS type signature \"%s\" to glib C type in method \"%s\" of interface \"%s\""),
                      arg_info_get_type (arg),
                      method_info_get_name (method),
                      interface_info_get_name (iface));
         return FALSE;
       }

      /* Variants are special...*/
      if (gtype == G_TYPE_VALUE)
	{
	  if (direction == ARG_IN)
	    type_suffix = "*";
	  else
	    type_suffix = "";
	}
      else if ((g_type_is_a (gtype, G_TYPE_BOXED)
	      || g_type_is_a (gtype, G_TYPE_OBJECT)
	   || g_type_is_a (gtype, G_TYPE_POINTER)))
	type_suffix = "*";
      else
	type_suffix = "";

      if (direction != dir)
        continue;

          switch (dir)
       {
       case ARG_IN:
         if (!write_printf_to_iochannel ("  %s%s IN_%s;\n", channel, error,
                                         type_str, type_suffix,
                                         arg_info_get_name (arg)))
           goto io_lose;
         break;
       case ARG_OUT:
         if (!write_printf_to_iochannel ("  %s%s OUT_%s;\n", channel, error,
                                         type_str, type_suffix,
                                         arg_info_get_name (arg)))
           goto io_lose;
         break;
       case ARG_INVALID:
         break;
       }
     }
   return TRUE;
 io_lose:
  return FALSE;
 }

static gboolean
write_formal_parameters_for_direction (InterfaceInfo *iface, MethodInfo *method, int dir, GIOChannel *channel, GError **error)
{
  GSList *args;

  for (args = method_info_get_args (method); args; args = args->next)
    {
      ArgInfo *arg;
      const char *type_str;
      const char *type_suffix;
      GType gtype;
      int direction;

      arg = args->data;

      direction = arg_info_get_direction (arg);
      if (dir != direction) continue;
      
      WRITE_OR_LOSE (", ");

      gtype = dbus_gtype_from_signature (arg_info_get_type (arg), TRUE);
      type_str = dbus_g_type_get_c_name (gtype);
      /* Variants are special...*/
      if (gtype == G_TYPE_VALUE)
	{
	  if (direction == ARG_IN)
	    type_suffix = "*";
	  else
	    type_suffix = "";
	}
      else if ((g_type_is_a (gtype, G_TYPE_BOXED)
	      || g_type_is_a (gtype, G_TYPE_OBJECT)
	   || g_type_is_a (gtype, G_TYPE_POINTER)))
	type_suffix = "*";
      else
	type_suffix = "";

      if (!type_str)
	{
	  g_set_error (error,
		       DBUS_BINDING_TOOL_ERROR,
		       DBUS_BINDING_TOOL_ERROR_UNSUPPORTED_CONVERSION,
		       _("Unsupported conversion from D-BUS type signature \"%s\" to glib C type in method \"%s\" of interface \"%s\""),
		       arg_info_get_type (arg),
		       method_info_get_name (method),
		       interface_info_get_name (iface));
	  return FALSE;
	}
 
       switch (direction)
 	{
 	case ARG_IN:
	  if (!write_printf_to_iochannel ("const %s%s IN_%s", channel, error,
					  type_str,
					  type_suffix,
					  arg_info_get_name (arg)))
 	    goto io_lose;
 	  break;
 	case ARG_OUT:
	  if (!write_printf_to_iochannel ("%s%s* OUT_%s", channel, error,
					  type_str,
					  type_suffix,
					  arg_info_get_name (arg)))
 	    goto io_lose;
 	  break;
 	case ARG_INVALID:
	  break;
	}
    }
  return TRUE;
 io_lose:
  return FALSE;
}

static gboolean
write_typed_args_for_direction (InterfaceInfo *iface, MethodInfo *method, GIOChannel *channel, const int direction, GError **error)
 {
  GSList *args;
  
  for (args = method_info_get_args (method); args; args = args->next)
    {
      ArgInfo *arg;
      int dir;
      GType gtype;
      const char *type_lookup;
      
      arg = args->data;

      dir = arg_info_get_direction (arg);

      if (dir != direction)
        continue;

      gtype = dbus_gtype_from_signature (arg_info_get_type (arg), TRUE);
      type_lookup = dbus_g_type_get_lookup_function (gtype);

      if (!write_printf_to_iochannel ("%s, &%s_%s, ", channel, error, type_lookup, direction == ARG_IN ? "IN" : "OUT", arg_info_get_name (arg)))
          goto io_lose;
    }
  return TRUE;
 io_lose:
  return FALSE;
}

static gboolean
write_async_method_client (GIOChannel *channel, InterfaceInfo *interface, MethodInfo *method, GError **error)
{
  char *method_name, *iface_prefix;
  iface_prefix = iface_to_c_prefix (interface_info_get_name (interface));
  method_name = compute_client_method_name (iface_prefix, method);
  
  /* Write the typedef for the client callback */
  if (!write_printf_to_iochannel ("typedef void (*%s_reply) (", channel, error, method_name))
    goto io_lose;
  {
    GSList *args;
    for (args = method_info_get_args (method); args; args = args->next)
      {
	ArgInfo *arg;
	const char *type_suffix, *type_str;
	GType gtype;
	
	arg = args->data;
	
	if (arg_info_get_direction (arg) != ARG_OUT)
	  continue;
	gtype = dbus_gtype_from_signature (arg_info_get_type (arg), TRUE);
	if (gtype != G_TYPE_VALUE && (g_type_is_a (gtype, G_TYPE_BOXED)
	     || g_type_is_a (gtype, G_TYPE_OBJECT)
	     || g_type_is_a (gtype, G_TYPE_POINTER)))
	  type_suffix = "*";
	else
	  type_suffix = "";
	type_str = dbus_g_type_get_c_name (dbus_gtype_from_signature (arg_info_get_type (arg), TRUE));
	if (!write_printf_to_iochannel ("%s %sOUT_%s, ", channel, error, type_str, type_suffix, arg_info_get_name (arg)))
	  goto io_lose;
      }
  }
  WRITE_OR_LOSE ("GError *error, gpointer userdata);\n\n");
  
  
  /* Write the callback when the call returns */
  WRITE_OR_LOSE ("static void\n");
  if (!write_printf_to_iochannel ("%s_async_callback (DBusGPendingCall *pending, DBusGAsyncData *data)\n", channel, error, method_name))
    goto io_lose;
  WRITE_OR_LOSE ("{\n");
  WRITE_OR_LOSE ("  GError *error = NULL;\n");
  if (!write_formal_declarations_for_direction (interface, method, channel, ARG_OUT, error))
    goto io_lose;
  WRITE_OR_LOSE ("  dbus_g_proxy_end_call (data->proxy, pending, &error, ");
  if (!write_typed_args_for_direction (interface, method, channel, ARG_OUT, error))
    goto io_lose;
  WRITE_OR_LOSE("G_TYPE_INVALID);\n");
  if (!write_printf_to_iochannel ("  (*(%s_reply)data->cb) (", channel, error, method_name))
    goto io_lose;
  if (!write_untyped_out_args (interface, method, channel, error))
    goto io_lose;
  WRITE_OR_LOSE ("error, data->userdata);\n");
  WRITE_OR_LOSE ("  return;\n}\n\n");
  

  /* Write the main wrapper function */
  WRITE_OR_LOSE ("static\n#ifdef G_HAVE_INLINE\ninline\n#endif\ngboolean\n");
  if (!write_printf_to_iochannel ("%s_async (DBusGProxy *proxy", channel, error,
                                  method_name))
    goto io_lose;
  if (!write_formal_parameters_for_direction (interface, method, ARG_IN, channel, error))
    goto io_lose;
  
  if (!write_printf_to_iochannel (", %s_reply callback, gpointer userdata)\n\n", channel, error, method_name))
    goto io_lose;
  
  WRITE_OR_LOSE ("{\n");
  WRITE_OR_LOSE ("  DBusGPendingCall *pending;\n  DBusGAsyncData *stuff;\n  stuff = g_new (DBusGAsyncData, 1);\n  stuff->proxy = proxy;\n  stuff->cb = callback;\n  stuff->userdata = userdata;\n");
  if (!write_printf_to_iochannel ("  pending = dbus_g_proxy_begin_call (proxy, \"%s\", ", channel, error, method_info_get_name (method)))
    goto io_lose;
  if (!write_args_for_direction (interface, method, channel, ARG_IN, error))
    goto io_lose;
  WRITE_OR_LOSE ("G_TYPE_INVALID);\n");

  if (!write_printf_to_iochannel ("  dbus_g_pending_call_set_notify(pending, (DBusGPendingCallNotify)%s_async_callback, stuff, g_free);\n", channel, error, method_name))
    goto io_lose;

  WRITE_OR_LOSE ("  return TRUE;\n}\n\n");

  g_free (method_name);
  return TRUE;
 io_lose:
  return FALSE;
 }

static gboolean
generate_client_glue_list (GSList *list, DBusBindingToolCData *data, GError **error)
{
  GSList *tmp;

  tmp = list;
  while (tmp != NULL)
    {
      if (!generate_client_glue (tmp->data, data, error))
	return FALSE;
      tmp = tmp->next;
    }
  return TRUE;
}

static gboolean
generate_client_glue (BaseInfo *base, DBusBindingToolCData *data, GError **error)
{
  if (base_info_get_type (base) == INFO_TYPE_NODE)
    {
      if (!generate_client_glue_list (node_info_get_nodes ((NodeInfo *) base),
				      data, error))
	return FALSE;
      if (!generate_client_glue_list (node_info_get_interfaces ((NodeInfo *) base),
				      data, error))
	return FALSE;
    }
  else
    {
      GIOChannel *channel;
      InterfaceInfo *interface;
      GSList *methods;
      GSList *tmp;
      int count;
      char *iface_prefix;

      channel = data->channel;

      interface = (InterfaceInfo *) base;

      methods = interface_info_get_methods (interface);
      count = 0;

      iface_prefix = iface_to_c_prefix (interface_info_get_name (interface));

      if (!write_printf_to_iochannel ("#ifndef DBUS_GLIB_CLIENT_WRAPPERS_%s\n"
				      "#define DBUS_GLIB_CLIENT_WRAPPERS_%s\n\n",
				      channel, error,
				      iface_prefix, iface_prefix))
	{
	  g_free (iface_prefix);
	  goto io_lose;
	}

      for (tmp = methods; tmp != NULL; tmp = g_slist_next (tmp))
        {
          MethodInfo *method;
	  char *method_name;

          method = (MethodInfo *) tmp->data;

	  if (data->ignore_unsupported && !check_supported_parameters (method))
	    {
	      g_warning ("Ignoring unsupported signature in method \"%s\" of interface \"%s\"\n",
			 method_info_get_name (method),
			 interface_info_get_name (interface));
	      continue;
	    }

	  method_name = compute_client_method_name (iface_prefix, method);

	  WRITE_OR_LOSE ("static\n#ifdef G_HAVE_INLINE\ninline\n#endif\ngboolean\n");
	  if (!write_printf_to_iochannel ("%s (DBusGProxy *proxy", channel, error,
					  method_name))
	    goto io_lose;
	  g_free (method_name);

	  if (!write_formal_parameters (interface, method, channel, error))
	    goto io_lose;

	  WRITE_OR_LOSE (", GError **error)\n\n");
	  
	  WRITE_OR_LOSE ("{\n");
	  
	  if (!write_printf_to_iochannel ("  return dbus_g_proxy_invoke (proxy, \"%s\", ", channel, error,
					  method_info_get_name (method)))
	    goto io_lose;

	  WRITE_OR_LOSE ("error, ");

	  if (!write_args_for_direction (interface, method, channel, ARG_IN, error))
	    goto io_lose;

	  WRITE_OR_LOSE ("G_TYPE_INVALID, ");

	  if (!write_args_for_direction (interface, method, channel, ARG_OUT, error))
	    goto io_lose;

	  WRITE_OR_LOSE ("G_TYPE_INVALID);\n}\n\n");

	  write_async_method_client (channel, interface, method, error);
	}

      if (!write_printf_to_iochannel ("#endif /* defined DBUS_GLIB_CLIENT_WRAPPERS_%s */\n\n", channel, error, iface_prefix))
	{
	  g_free (iface_prefix);
	  goto io_lose;
	}
    }
  return TRUE;
 io_lose:
  return FALSE;
}


gboolean
dbus_binding_tool_output_glib_client (BaseInfo *info, GIOChannel *channel, gboolean ignore_unsupported, GError **error)
{
  DBusBindingToolCData data;
  gboolean ret;

  dbus_g_value_types_init ();

  memset (&data, 0, sizeof (data));
  
  data.channel = channel;
  data.ignore_unsupported = ignore_unsupported;

  WRITE_OR_LOSE ("/* Generated by dbus-binding-tool; do not edit! */\n\n");
  WRITE_OR_LOSE ("#include <glib/gtypes.h>\n");
  WRITE_OR_LOSE ("#include <glib/gerror.h>\n");
  WRITE_OR_LOSE ("#include <dbus/dbus-glib.h>\n\n");
  WRITE_OR_LOSE ("G_BEGIN_DECLS\n\n");

  ret = generate_client_glue (info, &data, error);
  if (!ret)
    goto io_lose;
  
  WRITE_OR_LOSE ("G_END_DECLS\n");

  return ret;
 io_lose:
  return FALSE;
}
