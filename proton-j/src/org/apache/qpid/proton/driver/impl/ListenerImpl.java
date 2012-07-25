/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */
package org.apache.qpid.proton.driver.impl;

import java.io.IOException;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import org.apache.qpid.proton.driver.Connector;
import org.apache.qpid.proton.driver.Listener;

class ListenerImpl<C> implements Listener<C>
{
    private final C _context;
    private final ServerSocketChannel _channel;
    private final DriverImpl _driver;

    ListenerImpl(DriverImpl driver, ServerSocketChannel c, C context)
    {
        _driver = driver;
        _channel = c;
        _context = context;
    }

    public Connector accept()
    {
        try
        {
            SocketChannel c = _channel.accept();
            if(c != null)
            {
                c.configureBlocking(false);
                return _driver.createConnector(c, _context);
            }
        }
        catch (IOException e)
        {
            e.printStackTrace();  // TODO - Implement
        }
        return null;  //TODO - Implement
    }

    public C getContext()
    {
        return _context;
    }

    public void close()
    {
        //TODO - Implement
    }

    public void destroy()
    {
        //TODO - Implement
    }
}
