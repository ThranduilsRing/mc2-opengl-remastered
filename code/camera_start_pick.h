#pragma once
// Pure selection of the mover the camera starts on (GameCamera::activate). A mover whose commander
// slot is empty (Mover::getCommander() == nullptr, e.g. commanderId names a commander that was never
// created) is not the player's: skip it instead of dereferencing the null commander.

// Mover needs: getCommanderId(), isOnGUI(). Returns nullptr when numMovers == 0; when no mover matches,
// returns the last mover (the original loop's fall-through behaviour).
template <class Mover, class GetMover>
Mover* pickCameraStartMover(long numMovers, long homeCommanderId, GetMover getMover)
{
    Mover* first = nullptr;
    if (numMovers > 0) {
        long i = 0;
        first = getMover(i);
        while (first && ((first->getCommanderId() != homeCommanderId) || !first->isOnGUI())) {
            if (++i == numMovers)
                break;
            first = getMover(i);
        }
    }
    return first;
}
