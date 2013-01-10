/* This file is part of Clementine.
   Copyright 2012, Andreas Muttscheller <asfa194@gmail.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "core/logging.h"
#include "covers/currentartloader.h"
#include "playlist/playlistmanager.h"

#include "networkremote.h"

#include <QDataStream>
#include <QSettings>

const char* NetworkRemote::kSettingsGroup = "NetworkRemote";
const int NetworkRemote::kDefaultServerPort = 5500;
const int NetworkRemote::kProtocolBufferVersion = 1;

NetworkRemote::NetworkRemote(Application* app)
  : app_(app)
{
  signals_connected_ = false;
}


NetworkRemote::~NetworkRemote() {
  StopServer();
  delete incoming_data_parser_;
  delete outgoing_data_creator_;
}

void NetworkRemote::ReadSettings() {
  QSettings s;

  s.beginGroup(NetworkRemote::kSettingsGroup);
  use_remote_ = s.value("use_remote").toBool();
  port_       = s.value("port").toInt();

  // Use only non public ips must be true be default
  if (s.contains("only_non_public_ip")) {
    only_non_public_ip_ = s.value("only_non_public_ip").toBool();
  } else {
    only_non_public_ip_ = true;
  }

  if (port_ == 0) {
    port_ = kDefaultServerPort;
  }
  s.endGroup();
}

void NetworkRemote::SetupServer() {
  server_ = new QTcpServer();
  server_ipv6_ = new QTcpServer();
  incoming_data_parser_  = new IncomingDataParser(app_);
  outgoing_data_creator_ = new OutgoingDataCreator(app_);

  outgoing_data_creator_->SetClients(&clients_);

  connect(app_->current_art_loader(),
          SIGNAL(ArtLoaded(const Song&, const QString&, const QImage&)),
          outgoing_data_creator_,
          SLOT(CurrentSongChanged(const Song&, const QString&, const QImage&)));
}

void NetworkRemote::StartServer() {
  if (!app_) {
    qLog(Error) << "Start Server called without having an application!";
    return;
  }
  // Check if user desires to start a network remote server
  ReadSettings();
  if (!use_remote_) {
    qLog(Info) << "Network Remote deactivated";
    return;
  }

  qLog(Info) << "Starting network remote";

  connect(server_, SIGNAL(newConnection()), this, SLOT(AcceptConnection()));
  connect(server_ipv6_, SIGNAL(newConnection()), this, SLOT(AcceptConnection()));

  server_->listen(QHostAddress::Any, port_);
  server_ipv6_->listen(QHostAddress::AnyIPv6, port_);

  qLog(Info) << "Listening on port " << port_;
}

void NetworkRemote::StopServer() {
  if (server_->isListening()) {
    server_->close();
    server_ipv6_->close();
    clients_.clear();
  }
}

void NetworkRemote::ReloadSettings() {
  StopServer();
  StartServer();
}

void NetworkRemote::AcceptConnection() {
  if (!signals_connected_) {
    signals_connected_ = true;

    // Setting up the signals, but only once
    connect(incoming_data_parser_, SIGNAL(SendClementineInfos()),
            outgoing_data_creator_, SLOT(SendClementineInfos()));
    connect(incoming_data_parser_, SIGNAL(SendFirstData()),
            outgoing_data_creator_, SLOT(SendFirstData()));
    connect(incoming_data_parser_, SIGNAL(SendAllPlaylists()),
            outgoing_data_creator_, SLOT(SendAllPlaylists()));
    connect(incoming_data_parser_, SIGNAL(SendPlaylistSongs(int)),
            outgoing_data_creator_, SLOT(SendPlaylistSongs(int)));

    connect(app_->playlist_manager(), SIGNAL(ActiveChanged(Playlist*)),
            outgoing_data_creator_, SLOT(ActiveChanged(Playlist*)));
    connect(app_->playlist_manager(), SIGNAL(PlaylistChanged(Playlist*)),
            outgoing_data_creator_, SLOT(PlaylistChanged(Playlist*)));

    connect(app_->player(), SIGNAL(VolumeChanged(int)), outgoing_data_creator_,
            SLOT(VolumeChanged(int)));
    connect(app_->player()->engine(), SIGNAL(StateChanged(Engine::State)),
            outgoing_data_creator_, SLOT(StateChanged(Engine::State)));
  }

  if (server_->hasPendingConnections()) {
    QTcpSocket* client_socket = server_->nextPendingConnection();
    // Check if our ip is in private scope
    if (only_non_public_ip_
     && !IpIsPrivate(client_socket->peerAddress().toIPv4Address())) {
      qLog(Info) << "Got a connection from public ip" <<
                  client_socket->peerAddress().toString();
    } else {
      CreateRemoteClient(client_socket);
      // TODO: Check private ips for ipv6
    }

  } else {
    // No checks on ipv6
    CreateRemoteClient(server_ipv6_->nextPendingConnection());
  }
}

bool NetworkRemote::IpIsPrivate(int ip) {
  int private_local = QHostAddress("127.0.0.1").toIPv4Address();
  int private_a = QHostAddress("10.0.0.0").toIPv4Address();
  int private_b = QHostAddress("172.16.0.0").toIPv4Address();
  int private_c = QHostAddress("192.168.0.0").toIPv4Address();

  // Check if we have a private ip address
  if (ip == private_local
   || (ip >= private_a && ip < private_a + 16777216)
   || (ip >= private_b && ip < private_b + 1048576)
   || (ip >= private_c && ip < private_c + 65536)) {
    return true;
  } else {
    return false;
  }
}

void NetworkRemote::CreateRemoteClient(QTcpSocket *client_socket) {
  if (client_socket) {
    // Add the client to the list
    RemoteClient* client = new RemoteClient(app_, client_socket);
    clients_.push_back(client);

    // Connect the signal to parse data
    connect(client, SIGNAL(Parse(QByteArray)),
            incoming_data_parser_, SLOT(Parse(QByteArray)));
  }
}