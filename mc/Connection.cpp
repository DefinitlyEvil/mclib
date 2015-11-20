#include "Connection.h"

#include "Encryption.h"
#include "Compression.h"
#include "Packets/PacketDispatcher.h"
#include "Packets/PacketFactory.h"
#include "Network/Network.h"
#include "Network/TCPSocket.h"

namespace Minecraft {

Connection::Connection(Minecraft::Packets::PacketDispatcher* dispatcher)
        : Minecraft::Packets::PacketHandler(dispatcher),
        m_Encrypter(new Minecraft::EncryptionStrategyNone()),
        m_Compressor(new Minecraft::CompressionNone()),
        m_Socket(new Network::TCPSocket())
{
    using namespace Minecraft;

    dispatcher->RegisterHandler(Protocol::State::Login, Protocol::Login::Disconnect, this);
    dispatcher->RegisterHandler(Protocol::State::Login, Protocol::Login::EncryptionRequest, this);
    dispatcher->RegisterHandler(Protocol::State::Login, Protocol::Login::LoginSuccess, this);
    dispatcher->RegisterHandler(Protocol::State::Login, Protocol::Login::SetCompression, this);

    dispatcher->RegisterHandler(Protocol::State::Play, Protocol::Play::KeepAlive, this);
    dispatcher->RegisterHandler(Protocol::State::Play, Protocol::Play::PlayerPositionAndLook, this);
    dispatcher->RegisterHandler(Protocol::State::Play, Protocol::Play::Disconnect, this);

    m_Socket->SetBlocking(false);
}

Connection::~Connection() {
    GetDispatcher()->UnregisterHandler(this);
}

void Connection::HandlePacket(Minecraft::Packets::Inbound::KeepAlivePacket* packet) {
    Minecraft::Packets::Outbound::KeepAlivePacket response(packet->GetAliveId());
    SendPacket(&response);
}

void Connection::HandlePacket(Minecraft::Packets::Inbound::PlayerPositionAndLookPacket* packet) {
    using namespace Minecraft::Packets;

    // Used to verify position
    Outbound::PlayerPositionAndLookPacket response(packet->GetX(), packet->GetY(), packet->GetZ(),
        packet->GetYaw(), packet->GetPitch(), true);

    SendPacket(&response);

    Outbound::ClientStatusPacket status(Outbound::ClientStatusPacket::Action::PerformRespawn);
    SendPacket(&status);
}

void Connection::HandlePacket(Minecraft::Packets::Inbound::DisconnectPacket* packet) {
    m_Socket->Disconnect();

    NotifyListeners(&ConnectionListener::OnSocketStateChange, m_Socket->GetStatus());

    if (m_ProtocolState != Protocol::State::Play)
        NotifyListeners(&ConnectionListener::OnLogin, false);
}

void Connection::AuthenticateClient(const std::wstring& serverId, const std::string& sharedSecret, const std::string& pubkey) {
    bool success = true;
    std::string error = "";

    try {
        if (!m_Yggdrasil.Authenticate(m_Username, m_Password)) {
            error = "Failed to authenticate";
            success = false;
        }
    } catch (const Minecraft::YggdrasilException& e) {
        error = e.what();
        success = false;
    }

    try {
        if (!m_Yggdrasil.JoinServer(serverId, sharedSecret, pubkey)) {
            success = false;
            error = "Failed to join server through Yggdrasil.";
        }
    } catch (const Minecraft::YggdrasilException& e) {
        success = false;
        error = e.what();
    }

    NotifyListeners(&ConnectionListener::OnAuthentication, success, error);
}

void Connection::HandlePacket(Minecraft::Packets::Inbound::EncryptionRequestPacket* packet) {
    std::string pubkey = packet->GetPublicKey();
    std::string verify = packet->GetVerifyToken();

    Minecraft::EncryptionStrategyAES* aesEncrypter = new Minecraft::EncryptionStrategyAES(pubkey, verify);
    Minecraft::Packets::Outbound::EncryptionResponsePacket* encResp = aesEncrypter->GenerateResponsePacket();

    AuthenticateClient(packet->GetServerId().GetUTF16(), aesEncrypter->GetSharedSecret(), pubkey);

    SendPacket(encResp);

    delete m_Encrypter;
    m_Encrypter = aesEncrypter;
}

void Connection::HandlePacket(Minecraft::Packets::Inbound::LoginSuccessPacket* packet) {
    m_ProtocolState = Minecraft::Protocol::State::Play;

    NotifyListeners(&ConnectionListener::OnLogin, true);
}

void Connection::HandlePacket(Minecraft::Packets::Inbound::SetCompressionPacket* packet) {
    delete m_Compressor;
    m_Compressor = new Minecraft::CompressionZ(packet->GetMaxPacketSize());
}

bool Connection::Connect(const std::string& server, u16 port) {
    bool result = false;

    m_Server = server;
    m_Port = port;

    if (isdigit(m_Server.at(0))) {
        Network::IPAddress addr(m_Server);

        result = m_Socket->Connect(addr, m_Port);
    } else {
        auto addrs = Network::Dns::Resolve(m_Server);
        if (addrs.size() == 0) return false;

        for (auto addr : addrs) {
            if (m_Socket->Connect(addr, m_Port)) {
                result = true;
                break;
            }
        }
    }

    if (result)
        NotifyListeners(&ConnectionListener::OnSocketStateChange, m_Socket->GetStatus());

    return result;
}

Minecraft::Packets::Packet* Connection::CreatePacket(Minecraft::DataBuffer& buffer) {
    std::size_t readOffset = buffer.GetReadOffset();
    Minecraft::VarInt length;

    try {
        buffer >> length;
    } catch (const std::out_of_range&) {
        // This will happen when the buffer only contains part of the VarInt, 
        // so only part of the packet was received so far.
        // The buffer read offset isn't advanced when the exception is thrown, so no need to set it back to what it was.
        return nullptr;
    }

    if (buffer.GetRemaining() < (u32)length.GetInt()) {
        // Reset the read offset back to what it was because the full packet hasn't been received yet.
        buffer.SetReadOffset(readOffset);
        return nullptr;
    }

    Minecraft::DataBuffer decompressed = m_Compressor->Decompress(buffer, length.GetInt());

    return Minecraft::Packets::PacketFactory::CreatePacket(m_ProtocolState, decompressed, length.GetInt());
}

void Connection::CreatePacket() {
    Minecraft::DataBuffer buffer;

    m_Socket->Receive(buffer, 4096);

    if (m_Socket->GetStatus() != Network::Socket::Connected) {
        NotifyListeners(&ConnectionListener::OnSocketStateChange, m_Socket->GetStatus());
        return;
    }

    if (buffer.GetSize() == 0) return;

    m_HandleBuffer << m_Encrypter->Decrypt(buffer);

    Minecraft::Packets::Packet* packet = nullptr;

    do {
        try {
            packet = CreatePacket(m_HandleBuffer);
            if (packet) {
                this->GetDispatcher()->Dispatch(packet);
                Minecraft::Packets::PacketFactory::FreePacket(packet);
            } else {
                break;
            }
        } catch (const Minecraft::Protocol::UnfinishedProtocolException&) {
            // Ignore for now
        }
    } while (!m_HandleBuffer.IsFinished() && m_HandleBuffer.GetSize() > 0);

    if (m_HandleBuffer.IsFinished())
        m_HandleBuffer = Minecraft::DataBuffer();
    else if (m_HandleBuffer.GetReadOffset() != 0)
        m_HandleBuffer = Minecraft::DataBuffer(m_HandleBuffer, m_HandleBuffer.GetReadOffset());
}

void Connection::Login(const std::string& username, const std::string& password) {
    Minecraft::Packets::Outbound::HandshakePacket handshake(47, m_Server, m_Port, Minecraft::Protocol::State::Login);
    SendPacket(&handshake);

    Minecraft::Packets::Outbound::LoginStartPacket loginStart(username);
    SendPacket(&loginStart);

    m_Username = username;
    m_Password = password;

    m_ProtocolState = Minecraft::Protocol::State::Login;
}

void Connection::SendPacket(Minecraft::Packets::Packet* packet) {
    Minecraft::DataBuffer packetBuffer = packet->Serialize();
    Minecraft::DataBuffer compressed = m_Compressor->Compress(packetBuffer);
    Minecraft::DataBuffer encrypted = m_Encrypter->Encrypt(compressed);

    m_Socket->Send(encrypted);
}

} // ns Minecraft
