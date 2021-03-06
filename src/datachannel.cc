#include <node_buffer.h>

#include <stdint.h>
#include <iostream>
#include <string>

#include "talk/app/webrtc/jsep.h"
#include "webrtc/system_wrappers/interface/ref_count.h"

#include "common.h"
#include "datachannel.h"

using namespace node_webrtc;

v8::Persistent<v8::Function> DataChannel::constructor;
v8::Persistent<v8::Function> DataChannel::ArrayBufferConstructor;

DataChannelObserver::DataChannelObserver(talk_base::scoped_refptr<webrtc::DataChannelInterface> jingleDataChannel) {
  TRACE_CALL;
  uv_mutex_init(&lock);
  _jingleDataChannel = jingleDataChannel;
  _jingleDataChannel->RegisterObserver(this);
  TRACE_END;
}

DataChannelObserver::~DataChannelObserver() {
  _jingleDataChannel = NULL;
}

void DataChannelObserver::OnStateChange()
{
  TRACE_CALL;
  DataChannel::StateEvent* data = new DataChannel::StateEvent(_jingleDataChannel->state());
  QueueEvent(DataChannel::STATE, static_cast<void*>(data));
  TRACE_END;
}

void DataChannelObserver::OnMessage(const webrtc::DataBuffer& buffer)
{
  TRACE_CALL;
    DataChannel::MessageEvent* data = new DataChannel::MessageEvent(&buffer);
    QueueEvent(DataChannel::MESSAGE, static_cast<void*>(data));
  TRACE_END;
}

void DataChannelObserver::QueueEvent(DataChannel::AsyncEventType type, void* data) {
  TRACE_CALL;
  DataChannel::AsyncEvent evt;
  evt.type = type;
  evt.data = data;
  uv_mutex_lock(&lock);
  _events.push(evt);
  uv_mutex_unlock(&lock);
  TRACE_END;
}

DataChannel::DataChannel(node_webrtc::DataChannelObserver* observer)
: loop(uv_default_loop()),
  _observer(observer),
  _binaryType(DataChannel::ARRAY_BUFFER)
{
  uv_mutex_init(&lock);
  uv_async_init(loop, &async, Run);

  _jingleDataChannel = observer->_jingleDataChannel;
  _jingleDataChannel->RegisterObserver(this);

  async.data = this;

  // Re-queue cached observer events
  while(true) {
    uv_mutex_lock(&observer->lock);
    bool empty = observer->_events.empty();
    if(empty)
    {
      uv_mutex_unlock(&observer->lock);
      break;
    }
    AsyncEvent evt = observer->_events.front();
    observer->_events.pop();
    uv_mutex_unlock(&observer->lock);
    QueueEvent(evt.type, evt.data);
  }

  delete observer;
}

DataChannel::~DataChannel()
{
  TRACE_CALL;
  TRACE_END;
}

NAN_METHOD(DataChannel::New) {
  TRACE_CALL;
  NanScope();

  if(!args.IsConstructCall()) {
    return NanThrowTypeError("Use the new operator to construct the DataChannel.");
  }

  v8::Local<v8::External> _observer = v8::Local<v8::External>::Cast(args[0]);
  node_webrtc::DataChannelObserver* observer = static_cast<node_webrtc::DataChannelObserver*>(_observer->Value());

  DataChannel* obj = new DataChannel(observer);
  obj->Wrap( args.This() );
  //V8::AdjustAmountOfExternalAllocatedMemory(1024 * 1024);

  TRACE_END;
  NanReturnValue( args.This() );
}

void DataChannel::QueueEvent(AsyncEventType type, void* data)
{
  TRACE_CALL;
  AsyncEvent evt;
  evt.type = type;
  evt.data = data;
  uv_mutex_lock(&lock);
  _events.push(evt);
  uv_mutex_unlock(&lock);

  uv_async_send(&async);
  TRACE_END;
}

void DataChannel::Run(uv_async_t* handle, int status)
{
  NanScope();
  DataChannel* self = static_cast<DataChannel*>(handle->data);
  TRACE_CALL_P((uintptr_t)self);
  v8::Handle<v8::Object> dc = NanObjectWrapHandle(self);
  bool do_shutdown = false;

  while(true)
  {
    uv_mutex_lock(&self->lock);
    bool empty = self->_events.empty();
    if(empty)
    {
      uv_mutex_unlock(&self->lock);
      break;
    }
    AsyncEvent evt = self->_events.front();
    self->_events.pop();
    uv_mutex_unlock(&self->lock);

    TRACE_U("evt.type", evt.type);
    if(DataChannel::ERROR & evt.type)
    {
      DataChannel::ErrorEvent* data = static_cast<DataChannel::ErrorEvent*>(evt.data);
      v8::Local<v8::Function> callback = v8::Local<v8::Function>::Cast(dc->Get(v8::String::New("onerror")));
      v8::Local<v8::Value> argv[1];
      argv[0] = v8::Exception::Error(v8::String::New(data->msg.c_str()));
      callback->Call(dc, 1, argv);
    } else if(DataChannel::STATE & evt.type)
    {
      StateEvent* data = static_cast<StateEvent*>(evt.data);
      v8::Local<v8::Function> callback = v8::Local<v8::Function>::Cast(dc->Get(v8::String::New("onstatechange")));
      v8::Local<v8::Value> argv[1];
      v8::Local<v8::Integer> state = v8::Uint32::New(data->state);
      argv[0] = state;
      callback->Call(dc, 1, argv);

      if(webrtc::DataChannelInterface::kClosed == self->_jingleDataChannel->state()) {
        do_shutdown = true;
      }
    } else if(DataChannel::MESSAGE & evt.type)
    {
      MessageEvent* data = static_cast<MessageEvent*>(evt.data);
      v8::Local<v8::Function> callback = v8::Local<v8::Function>::Cast(dc->Get(v8::String::New("onmessage")));

      v8::Local<v8::Value> argv[1];

      if(data->binary) {
        v8::Local<v8::Object> array = NanPersistentToLocal(ArrayBufferConstructor)->NewInstance();
        array->SetIndexedPropertiesToExternalArrayData(data->message, v8::kExternalByteArray, data->size);
        v8::Local<v8::String> byteLenghtKey = v8::String::New("byteLength");
        v8::Local<v8::Integer> byteLengthValue = v8::Uint32::New(data->size);
        array->ForceSet(byteLenghtKey, byteLengthValue);
        //V8::AdjustAmountOfExternalAllocatedMemory(data->size);

        argv[0] = array;
        callback->Call(dc, 1, argv);
      } else {
        v8::Local<v8::String> str = v8::String::New(data->message, data->size);

        argv[0] = str;
        callback->Call(dc, 1, argv);
      }
    }
    // FIXME: delete event
  }

  if(do_shutdown) {
    uv_close((uv_handle_t*)(&self->async), NULL);
  }

  TRACE_END;
}

void DataChannel::OnStateChange()
{
  TRACE_CALL;
  StateEvent* data = new StateEvent(_jingleDataChannel->state());
  QueueEvent(DataChannel::STATE, static_cast<void*>(data));
  TRACE_END;
}

void DataChannel::OnMessage(const webrtc::DataBuffer& buffer)
{
  TRACE_CALL;
  MessageEvent* data = new MessageEvent(&buffer);
  QueueEvent(DataChannel::MESSAGE, static_cast<void*>(data));
  TRACE_END;
}

NAN_METHOD(DataChannel::Send) {
  TRACE_CALL;
  NanScope();

  DataChannel* self = ObjectWrap::Unwrap<DataChannel>( args.This() );

  if(args[0]->IsString()) {
    v8::Local<v8::String> str = v8::Local<v8::String>::Cast(args[0]);
    std::string data = *v8::String::Utf8Value(str);

    webrtc::DataBuffer buffer(data);
    self->_jingleDataChannel->Send(buffer);
  } else {
    v8::Local<v8::Object> arraybuffer = v8::Local<v8::Object>::Cast(args[0]);
    void* data = arraybuffer->GetIndexedPropertiesExternalArrayData();
    uint32_t data_len = arraybuffer->GetIndexedPropertiesExternalArrayDataLength();

    talk_base::Buffer buffer(data, data_len);
    webrtc::DataBuffer data_buffer(buffer, true);
    self->_jingleDataChannel->Send(data_buffer);
  }

  TRACE_END;
  NanReturnValue(v8::Undefined());
}

NAN_METHOD(DataChannel::Close) {
  TRACE_CALL;
  NanScope();

  DataChannel* self = ObjectWrap::Unwrap<DataChannel>( args.This() );
  self->_jingleDataChannel->Close();

  TRACE_END;
  NanReturnValue(v8::Undefined());
}

NAN_METHOD(DataChannel::Shutdown) {
  TRACE_CALL;
  NanScope();

  DataChannel* self = ObjectWrap::Unwrap<DataChannel>( args.This() );
  if(!uv_is_closing((uv_handle_t*)(&self->async)))
    uv_close((uv_handle_t*)(&self->async), NULL);

  TRACE_END;
  NanReturnValue(v8::Undefined());
}

NAN_GETTER(DataChannel::GetLabel) {
  TRACE_CALL;
  NanScope();

  DataChannel* self = node::ObjectWrap::Unwrap<DataChannel>( args.Holder() );

  std::string label = self->_jingleDataChannel->label();

  TRACE_END;
  NanReturnValue(v8::String::New(label.c_str()));
}

NAN_GETTER(DataChannel::GetReadyState) {
  TRACE_CALL;
  NanScope();

  DataChannel* self = node::ObjectWrap::Unwrap<DataChannel>( args.Holder() );

  webrtc::DataChannelInterface::DataState state = self->_jingleDataChannel->state();

  TRACE_END;
  NanReturnValue(v8::Number::New(static_cast<uint32_t>(state)));
}

NAN_GETTER(DataChannel::GetBinaryType) {
  TRACE_CALL;
  NanScope();

  DataChannel* self = node::ObjectWrap::Unwrap<DataChannel>( args.Holder() );

  TRACE_END;
  NanReturnValue(v8::Number::New(static_cast<uint32_t>(self->_binaryType)));
}

NAN_SETTER(DataChannel::SetBinaryType) {
  TRACE_CALL;

  DataChannel* self = node::ObjectWrap::Unwrap<DataChannel>( args.Holder() );
  self->_binaryType = static_cast<BinaryType>(value->Uint32Value());

  TRACE_END;
}

NAN_SETTER(DataChannel::ReadOnly) {
  INFO("PeerConnection::ReadOnly");
}

void DataChannel::Init( v8::Handle<v8::Object> exports ) {
  v8::Local<v8::FunctionTemplate> tpl = v8::FunctionTemplate::New( New );
  tpl->SetClassName( v8::String::NewSymbol( "DataChannel" ) );
  tpl->InstanceTemplate()->SetInternalFieldCount(1);
  tpl->PrototypeTemplate()->Set( v8::String::NewSymbol( "close" ),
    v8::FunctionTemplate::New( Close )->GetFunction() );
  tpl->PrototypeTemplate()->Set( v8::String::NewSymbol( "shutdown" ),
    v8::FunctionTemplate::New( Shutdown )->GetFunction() );

  tpl->PrototypeTemplate()->Set( v8::String::NewSymbol( "send" ),
    v8::FunctionTemplate::New( Send )->GetFunction() );

  tpl->InstanceTemplate()->SetAccessor(v8::String::New("label"), GetLabel, ReadOnly);
  tpl->InstanceTemplate()->SetAccessor(v8::String::New("binaryType"), GetBinaryType, SetBinaryType);
  tpl->InstanceTemplate()->SetAccessor(v8::String::New("readyState"), GetReadyState, ReadOnly);

  NanAssignPersistent(v8::Function, constructor, tpl->GetFunction());
  exports->Set( v8::String::NewSymbol("DataChannel"), tpl->GetFunction() );

  v8::Local<v8::Object> global = v8::Context::GetCurrent()->Global();
  v8::Local<v8::Value> obj = global->Get(v8::String::New("ArrayBuffer"));
  NanAssignPersistent(v8::Function, ArrayBufferConstructor, obj.As<v8::Function>());
}
