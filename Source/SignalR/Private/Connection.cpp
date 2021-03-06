// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Connection.h"
#include "HttpModule.h"
#include "IHttpRequest.h"
#include "WebSocketsModule.h"
#include "SignalRModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IHttpRequest.h"
#include "JsonObject.h"
#include "JsonReader.h"
#include "JsonSerializer.h"

FConnection::FConnection(const FString& InHost, const TMap<FString, FString>& InHeaders):
    Host(InHost),
    Headers(InHeaders)
{
}

void FConnection::Connect()
{
    Negotiate();
}

bool FConnection::IsConnected()
{
    return Connection.IsValid() && Connection->IsConnected();
}

void FConnection::Send(const FString& Data)
{
    if (Connection.IsValid())
    {
        Connection->Send(Data);
    }
    else
    {
        UE_LOG(LogSignalR, Error, TEXT("Cannot send data to non connected websocket."));
    }
}

void FConnection::Close(int32 Code, const FString& Reason)
{
    if(Connection.IsValid())
    {
        Connection->Close(Code, Reason);
    }
    else
    {
        UE_LOG(LogSignalR, Error, TEXT("Cannot close non connected websocket."));
    }
}

IWebSocket::FWebSocketConnectedEvent& FConnection::OnConnected()
{
    return OnConnectedEvent;
}

IWebSocket::FWebSocketConnectionErrorEvent& FConnection::OnConnectionError()
{
    return OnConnectionErrorEvent;
}

IWebSocket::FWebSocketClosedEvent& FConnection::OnClosed()
{
    return OnClosedEvent;
}

IWebSocket::FWebSocketMessageEvent& FConnection::OnMessage()
{
    return OnMessageEvent;
}

void FConnection::Negotiate()
{
    TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();

    HttpRequest->SetVerb(TEXT("POST"));
    HttpRequest->OnProcessRequestComplete().BindRaw(this, &FConnection::OnNegotiateResponse);
    HttpRequest->SetURL(TEXT("http://") + Host + TEXT("/negotiate?negotiateVersion=1"));
    HttpRequest->ProcessRequest();
}

void FConnection::OnNegotiateResponse(FHttpRequestPtr InRequest, FHttpResponsePtr InResponse, bool bConnectedSuccessfully)
{
    if(InResponse->GetResponseCode() != 200)
    {
        UE_LOG(LogSignalR, Error, TEXT("Negotiate failed with status code %d"), InResponse->GetResponseCode());
        return;
    }

    TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject());
    TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(InResponse->GetContentAsString());

    if (FJsonSerializer::Deserialize(JsonReader, JsonObject) && JsonObject.IsValid())
    {
        if(JsonObject->HasField(TEXT("error")))
        {
            // TODO: error
        }
        else
        {
            if (JsonObject->HasField(TEXT("ProtocolVersion")))
            {
                UE_LOG(LogSignalR, Error, TEXT("Detected a connection attempt to an ASP.NET SignalR Server. This client only supports connecting to an ASP.NET Core SignalR Server. See https://aka.ms/signalr-core-differences for details."));
                return;
            }

            if (JsonObject->HasTypedField<EJson::String>(TEXT("url")))
            {
                FString RedirectionUrl = JsonObject->GetStringField(TEXT("url"));
                FString AccessToken = JsonObject->GetStringField(TEXT("accessToken"));
                // TODO: redirection
                return;
            }

            if (JsonObject->HasTypedField<EJson::Array>(TEXT("availableTransports")))
            {
                // check if support WebSockets with Text format
                bool bIsCompatible = false;
                for (TSharedPtr<FJsonValue> TransportData : JsonObject->GetArrayField(TEXT("availableTransports")))
                {
                    if(TransportData.IsValid() && TransportData->Type == EJson::Object)
                    {
                        TSharedPtr<FJsonObject> TransportObj = TransportData->AsObject();
                        if(TransportObj->HasTypedField<EJson::String>(TEXT("transport")) && TransportObj->GetStringField(TEXT("transport")) == TEXT("WebSockets") && TransportObj->HasTypedField<EJson::Array>(TEXT("transferFormats")))
                        {
                            for (TSharedPtr<FJsonValue> TransportFormatData : TransportObj->GetArrayField(TEXT("transferFormats")))
                            {
                                if (TransportFormatData.IsValid() && TransportFormatData->Type == EJson::String && TransportFormatData->AsString() == TEXT("Text"))
                                {
                                    bIsCompatible = true;
                                }
                            }
                        }
                    }
                }

                if(!bIsCompatible)
                {
                    UE_LOG(LogSignalR, Error, TEXT("The server does not support WebSockets which is currently the only transport supported by this client."));
                    return;
                }
            }

            if (JsonObject->HasTypedField<EJson::String>(TEXT("connectionId")))
            {
                ConnectionId = JsonObject->GetStringField(TEXT("connectionId"));
            }

            if (JsonObject->HasTypedField<EJson::String>(TEXT("connectionToken")))
            {
                ConnectionId = JsonObject->GetStringField(TEXT("connectionToken"));
            }

            StartWebSocket();
        }
    }
    else
    {
        UE_LOG(LogSignalR, Error, TEXT("Cannot parse negotiate response: %s"), *InResponse->GetContentAsString());
    }
}

void FConnection::StartWebSocket()
{
    Connection = FWebSocketsModule::Get().CreateWebSocket(TEXT("ws://") + Host, FString(), Headers);

    if(Connection.IsValid())
    {
        Connection->OnConnected().AddLambda([this]() { OnConnectedEvent.Broadcast(); });
        Connection->OnConnectionError().AddLambda([this](const FString& ErrString)
        {
            UE_LOG(LogSignalR, Warning, TEXT("Websocket err: %s"), *ErrString);
            OnConnectionErrorEvent.Broadcast(ErrString);
        });
        Connection->OnClosed().AddLambda([this](int32 StatusCode, const FString& Reason, bool bWasClean) { OnClosedEvent.Broadcast(StatusCode, Reason, bWasClean); });
        Connection->OnMessage().AddLambda([this](const FString& MessageString) { OnMessageEvent.Broadcast(MessageString); });

        Connection->Connect();
    }
    else
    {
        UE_LOG(LogSignalR, Error, TEXT("Cannot start websocket."));
    }
}
