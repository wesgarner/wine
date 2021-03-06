/*
 * Copyright 2009 Matteo Bruni
 * Copyright 2010 Matteo Bruni for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS
#include "config.h"
#include "wine/port.h"
#include "wine/debug.h"
#include "wine/unicode.h"

#include "d3dcompiler_private.h"
#include "wine/wpp.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3dcompiler);

#define D3DXERR_INVALIDDATA                      0x88760b59

#define BUFFER_INITIAL_CAPACITY 256

struct mem_file_desc
{
    const char *buffer;
    unsigned int size;
    unsigned int pos;
};

struct mem_file_desc current_shader;
LPD3DINCLUDE current_include;

#define INCLUDES_INITIAL_CAPACITY 4

struct loaded_include
{
    const char *name;
    const char *data;
};

struct loaded_include *includes;
int includes_capacity, includes_size;
const char *parent_include;

char *wpp_output;
int wpp_output_capacity, wpp_output_size;

char *wpp_messages;
int wpp_messages_capacity, wpp_messages_size;

/* Mutex used to guarantee a single invocation
   of the D3DXAssembleShader function (or its variants) at a time.
   This is needed as wpp isn't thread-safe */
static CRITICAL_SECTION wpp_mutex;
static CRITICAL_SECTION_DEBUG wpp_mutex_debug =
{
    0, 0, &wpp_mutex,
    { &wpp_mutex_debug.ProcessLocksList,
      &wpp_mutex_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": wpp_mutex") }
};
static CRITICAL_SECTION wpp_mutex = { &wpp_mutex_debug, -1, 0, 0, 0, 0 };

/* Preprocessor error reporting functions */
static void wpp_write_message(const char *fmt, va_list args)
{
    char* newbuffer;
    int rc, newsize;

    if(wpp_messages_capacity == 0)
    {
        wpp_messages = HeapAlloc(GetProcessHeap(), 0, MESSAGEBUFFER_INITIAL_SIZE);
        if(wpp_messages == NULL)
        {
            ERR("Error allocating memory for parser messages\n");
            return;
        }
        wpp_messages_capacity = MESSAGEBUFFER_INITIAL_SIZE;
    }

    while(1)
    {
        rc = vsnprintf(wpp_messages + wpp_messages_size,
                       wpp_messages_capacity - wpp_messages_size, fmt, args);

        if (rc < 0 ||                                           /* C89 */
            rc >= wpp_messages_capacity - wpp_messages_size) {  /* C99 */
            /* Resize the buffer */
            newsize = wpp_messages_capacity * 2;
            newbuffer = HeapReAlloc(GetProcessHeap(), 0, wpp_messages, newsize);
            if(newbuffer == NULL)
            {
                ERR("Error reallocating memory for parser messages\n");
                return;
            }
            wpp_messages = newbuffer;
            wpp_messages_capacity = newsize;
        }
        else
        {
            wpp_messages_size += rc;
            return;
        }
    }
}

static void PRINTF_ATTR(1,2) wpp_write_message_var(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    wpp_write_message(fmt, args);
    va_end(args);
}

static void wpp_error(const char *file, int line, int col, const char *near,
                      const char *msg, va_list ap)
{
    wpp_write_message_var("%s:%d:%d: %s: ", file ? file : "'main file'",
                          line, col, "Error");
    wpp_write_message(msg, ap);
    wpp_write_message_var("\n");
}

static void wpp_warning(const char *file, int line, int col, const char *near,
                        const char *msg, va_list ap)
{
    wpp_write_message_var("%s:%d:%d: %s: ", file ? file : "'main file'",
                          line, col, "Warning");
    wpp_write_message(msg, ap);
    wpp_write_message_var("\n");
}

static char *wpp_lookup_mem(const char *filename, const char *parent_name,
                            char **include_path, int include_path_count)
{
    /* Here we return always ok. We will maybe fail on the next wpp_open_mem */
    char *path;
    int i;

    parent_include = NULL;
    if(parent_name[0] != '\0')
    {
        for(i = 0; i < includes_size; i++)
        {
            if(!strcmp(parent_name, includes[i].name))
            {
                parent_include = includes[i].data;
                break;
            }
        }
        if(parent_include == NULL)
        {
            ERR("Parent include file missing\n");
            return NULL;
        }
    }

    path = malloc(strlen(filename) + 1);
    if(path)
        memcpy(path, filename, strlen(filename) + 1);
    return path;
}

static void *wpp_open_mem(const char *filename, int type)
{
    struct mem_file_desc *desc;
    HRESULT hr;

    if(filename[0] == '\0') /* "" means to load the initial shader */
    {
        current_shader.pos = 0;
        return &current_shader;
    }

    if(current_include == NULL) return NULL;
    desc = HeapAlloc(GetProcessHeap(), 0, sizeof(*desc));
    if(!desc)
    {
        ERR("Error allocating memory\n");
        return NULL;
    }
    hr = ID3DInclude_Open(current_include,
                          type ? D3D_INCLUDE_SYSTEM : D3D_INCLUDE_LOCAL,
                          filename, parent_include, (LPCVOID *)&desc->buffer,
                          &desc->size);
    if(FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, desc);
        return NULL;
    }

    if(includes_capacity == includes_size)
    {
        if(includes_capacity == 0)
        {
            includes = HeapAlloc(GetProcessHeap(), 0, INCLUDES_INITIAL_CAPACITY);
            if(includes == NULL)
            {
                ERR("Error allocating memory for the loaded includes structure\n");
                goto error;
            }
            includes_capacity = INCLUDES_INITIAL_CAPACITY;
        }
        else
        {
            int newcapacity = includes_capacity * 2;
            struct loaded_include *newincludes =
                HeapReAlloc(GetProcessHeap(), 0, includes, newcapacity);
            if(newincludes == NULL)
            {
                ERR("Error reallocating memory for the loaded includes structure\n");
                goto error;
            }
            includes = newincludes;
            includes_capacity = newcapacity;
        }
    }
    includes[includes_size].name = filename;
    includes[includes_size++].data = desc->buffer;

    desc->pos = 0;
    return desc;

error:
    ID3DInclude_Close(current_include, desc->buffer);
    HeapFree(GetProcessHeap(), 0, desc);
    return NULL;
}

static void wpp_close_mem(void *file)
{
    struct mem_file_desc *desc = file;

    if(desc != &current_shader)
    {
        if(current_include)
            ID3DInclude_Close(current_include, desc->buffer);
        else
            ERR("current_include == NULL, desc == %p, buffer = %s\n",
                desc, desc->buffer);

        HeapFree(GetProcessHeap(), 0, desc);
    }
}

static int wpp_read_mem(void *file, char *buffer, unsigned int len)
{
    struct mem_file_desc *desc = file;

    len = min(len, desc->size - desc->pos);
    memcpy(buffer, desc->buffer + desc->pos, len);
    desc->pos += len;
    return len;
}

static void wpp_write_mem(const char *buffer, unsigned int len)
{
    char *new_wpp_output;

    if(wpp_output_capacity == 0)
    {
        wpp_output = HeapAlloc(GetProcessHeap(), 0, BUFFER_INITIAL_CAPACITY);
        if(!wpp_output)
        {
            ERR("Error allocating memory\n");
            return;
        }
        wpp_output_capacity = BUFFER_INITIAL_CAPACITY;
    }
    if(len > wpp_output_capacity - wpp_output_size)
    {
        while(len > wpp_output_capacity - wpp_output_size)
        {
            wpp_output_capacity *= 2;
        }
        new_wpp_output = HeapReAlloc(GetProcessHeap(), 0, wpp_output,
                                     wpp_output_capacity);
        if(!new_wpp_output)
        {
            ERR("Error allocating memory\n");
            return;
        }
        wpp_output = new_wpp_output;
    }
    memcpy(wpp_output + wpp_output_size, buffer, len);
    wpp_output_size += len;
}

static int wpp_close_output(void)
{
    char *new_wpp_output = HeapReAlloc(GetProcessHeap(), 0, wpp_output,
                                       wpp_output_size + 1);
    if(!new_wpp_output) return 0;
    wpp_output = new_wpp_output;
    wpp_output[wpp_output_size]='\0';
    return 1;
}

static HRESULT preprocess_shader(const void *data, SIZE_T data_size,
        const D3D_SHADER_MACRO *defines, ID3DInclude *include, ID3DBlob **error_messages)
{
    int ret;
    HRESULT hr = S_OK;
    const D3D_SHADER_MACRO *def = defines;

    static const struct wpp_callbacks wpp_callbacks =
    {
        wpp_lookup_mem,
        wpp_open_mem,
        wpp_close_mem,
        wpp_read_mem,
        wpp_write_mem,
        wpp_error,
        wpp_warning,
    };

    if (def != NULL)
    {
        while (def->Name != NULL)
        {
            wpp_add_define(def->Name, def->Definition);
            def++;
        }
    }
    current_include = include;
    includes_size = 0;

    wpp_output_size = wpp_output_capacity = 0;
    wpp_output = NULL;

    wpp_set_callbacks(&wpp_callbacks);
    wpp_messages_size = wpp_messages_capacity = 0;
    wpp_messages = NULL;
    current_shader.buffer = data;
    current_shader.size = data_size;

    ret = wpp_parse("", NULL);
    if (!wpp_close_output())
        ret = 1;
    if (ret)
    {
        TRACE("Error during shader preprocessing\n");
        if (wpp_messages)
        {
            int size;
            ID3DBlob *buffer;

            TRACE("Preprocessor messages:\n%s", wpp_messages);

            if (error_messages)
            {
                size = strlen(wpp_messages) + 1;
                hr = D3DCreateBlob(size, &buffer);
                if (FAILED(hr))
                    goto cleanup;
                CopyMemory(ID3D10Blob_GetBufferPointer(buffer), wpp_messages, size);
                *error_messages = buffer;
            }
        }
        if (data)
            TRACE("Shader source:\n%s\n", debugstr_an(data, data_size));
        hr = E_FAIL;
    }

cleanup:
    /* Remove the previously added defines */
    if (defines != NULL)
    {
        while (defines->Name != NULL)
        {
            wpp_del_define(defines->Name);
            defines++;
        }
    }
    HeapFree(GetProcessHeap(), 0, wpp_messages);
    return hr;
}

static HRESULT assemble_shader(const char *preprocShader, const char *preprocMessages,
                               LPD3DBLOB* ppShader, LPD3DBLOB* ppErrorMsgs)
{
    struct bwriter_shader *shader;
    char *messages = NULL;
    HRESULT hr;
    DWORD *res;
    LPD3DBLOB buffer;
    int size;
    char *pos;

    shader = SlAssembleShader(preprocShader, &messages);

    if(messages || preprocMessages)
    {
        if(preprocMessages)
        {
            TRACE("Preprocessor messages:\n");
            TRACE("%s", preprocMessages);
        }
        if(messages)
        {
            TRACE("Assembler messages:\n");
            TRACE("%s", messages);
        }

        TRACE("Shader source:\n");
        TRACE("%s\n", debugstr_a(preprocShader));

        if(ppErrorMsgs)
        {
            size = (messages ? strlen(messages) : 0) +
                (preprocMessages ? strlen(preprocMessages) : 0) + 1;
            hr = D3DCreateBlob(size, &buffer);
            if(FAILED(hr))
            {
                HeapFree(GetProcessHeap(), 0, messages);
                if(shader) SlDeleteShader(shader);
                return hr;
            }
            pos = ID3D10Blob_GetBufferPointer(buffer);
            if(preprocMessages)
            {
                CopyMemory(pos, preprocMessages, strlen(preprocMessages) + 1);
                pos += strlen(preprocMessages);
            }
            if(messages)
                CopyMemory(pos, messages, strlen(messages) + 1);

            *ppErrorMsgs = buffer;
        }

        HeapFree(GetProcessHeap(), 0, messages);
    }

    if(shader == NULL)
    {
        ERR("Asm reading failed\n");
        return D3DXERR_INVALIDDATA;
    }

    hr = SlWriteBytecode(shader, 9, &res);
    SlDeleteShader(shader);
    if(FAILED(hr))
    {
        ERR("SlWriteBytecode failed with 0x%08x\n", hr);
        return D3DXERR_INVALIDDATA;
    }

    if(ppShader)
    {
        size = HeapSize(GetProcessHeap(), 0, res);
        hr = D3DCreateBlob(size, &buffer);
        if(FAILED(hr))
        {
            HeapFree(GetProcessHeap(), 0, res);
            return hr;
        }
        CopyMemory(ID3D10Blob_GetBufferPointer(buffer), res, size);
        *ppShader = buffer;
    }

    HeapFree(GetProcessHeap(), 0, res);

    return S_OK;
}

HRESULT WINAPI D3DAssemble(const void *data, SIZE_T datasize, const char *filename,
        const D3D_SHADER_MACRO *defines, ID3DInclude *include, UINT flags,
        ID3DBlob **shader, ID3DBlob **error_messages)
{
    HRESULT hr;

    EnterCriticalSection(&wpp_mutex);

    /* TODO: flags */
    if (flags) FIXME("flags %x\n", flags);

    if (shader) *shader = NULL;
    if (error_messages) *error_messages = NULL;

    hr = preprocess_shader(data, datasize, defines, include, error_messages);
    if (SUCCEEDED(hr))
        hr = assemble_shader(wpp_output, wpp_messages, shader, error_messages);

    HeapFree(GetProcessHeap(), 0, wpp_output);
    LeaveCriticalSection(&wpp_mutex);
    return hr;
}

HRESULT WINAPI D3DCompile(const void *data, SIZE_T data_size, const char *filename,
        const D3D_SHADER_MACRO *defines, ID3DInclude *include, const char *entrypoint,
        const char *target, UINT sflags, UINT eflags, ID3DBlob **shader, ID3DBlob **error_messages)
{
    FIXME("data %p, data_size %lu, filename %s, defines %p, include %p, entrypoint %s,\n"
            "target %s, sflags %#x, eflags %#x, shader %p, error_messages %p stub!\n",
            data, data_size, debugstr_a(filename), defines, include, debugstr_a(entrypoint),
            debugstr_a(target), sflags, eflags, shader, error_messages);

    TRACE("Shader source:\n%s\n", debugstr_an(data, data_size));

    if (error_messages)
        D3DCreateBlob(1, error_messages); /* zero fill used as string end */

    return D3DERR_INVALIDCALL;
}

HRESULT WINAPI D3DPreprocess(const void *data, SIZE_T size, const char *filename,
        const D3D_SHADER_MACRO *defines, ID3DInclude *include,
        ID3DBlob **shader, ID3DBlob **error_messages)
{
    HRESULT hr;
    ID3DBlob *buffer;

    if (!data)
        return E_INVALIDARG;

    EnterCriticalSection(&wpp_mutex);

    if (shader) *shader = NULL;
    if (error_messages) *error_messages = NULL;

    hr = preprocess_shader(data, size, defines, include, error_messages);

    if (SUCCEEDED(hr))
    {
        if (shader)
        {
            hr = D3DCreateBlob(wpp_output_size, &buffer);
            if (FAILED(hr))
                goto cleanup;
            CopyMemory(ID3D10Blob_GetBufferPointer(buffer), wpp_output, wpp_output_size);
            *shader = buffer;
        }
        else
            hr = E_INVALIDARG;
    }

cleanup:
    HeapFree(GetProcessHeap(), 0, wpp_output);
    LeaveCriticalSection(&wpp_mutex);
    return hr;
}
