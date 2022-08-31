# This file is part of Cockpit.
#
# Copyright (C) 2022 Red Hat, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

import asyncio
import logging

from ..channel import AsyncChannel

logger = logging.getLogger(__name__)


class ChattyChannel(AsyncChannel):
    payload = 'chatty1'

    async def run(self, options):
        await self.write(f"You sent options! {options}".encode('utf-8'))

        while True:
            await self.write("You still there?\n".encode('utf-8'))

            try:
                await asyncio.wait_for(self.read(), 1)
                return
            except asyncio.TimeoutError:
                continue
